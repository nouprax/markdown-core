#!/usr/bin/env node
import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
    MAX_RESULT_BYTES,
    ORDER,
    renderComparison,
    validateComparison,
    validateMeasurement,
    workload
} from "./pr-benchmark-result.mjs";

const root = path.resolve(import.meta.dirname, "..");
const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const fileDigest = (file) => digest(fs.readFileSync(file));
const run = (command, args, options = {}) =>
    execFileSync(command, args, { timeout: 120000, maxBuffer: 128 * 1024 * 1024, ...options });

function buildLibrary(repository, sha, destination, output, cc, lane) {
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "markdown-core-benchmark-build-"));
    try {
        const source = path.join(temporary, "source");
        const build = path.join(temporary, "build");
        fs.mkdirSync(source);
        const archive = run("git", ["-c", "core.fsmonitor=false", "archive", sha], { cwd: repository });
        run("tar", ["-x", "-C", source], { input: archive });
        const configure = [
            "-S",
            source,
            "-B",
            build,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DMARKDOWN_CORE_TESTS=OFF",
            "-DMARKDOWN_CORE_SHARED=ON",
            "-DMARKDOWN_CORE_BENCHMARKS=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DCMAKE_C_COMPILER=${cc}`,
            "-DCMAKE_C_COMPILER_LAUNCHER=",
            "-DCMAKE_C_FLAGS=",
            "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG"
        ];
        fs.writeFileSync(
            path.join(output, `${lane}-build.log`),
            run("cmake", configure, { stdio: ["ignore", "pipe", "pipe"] })
        );
        fs.appendFileSync(
            path.join(output, `${lane}-build.log`),
            run("cmake", ["--build", build, "--target", "libmarkdown-core-elements", "--parallel", "2"], {
                timeout: 600000
            })
        );
        const products = path.join(build, "packages/markdown-core/elements");
        const libraries = fs
            .readdirSync(products, { withFileTypes: true })
            .filter(
                (entry) => entry.isFile() && /^libmarkdown-core(?:\.so(?:\.\d+)*|(?:\.\d+)*\.dylib)$/.test(entry.name)
            );
        assert.equal(libraries.length, 1, "expected one public shared library");
        const library = path.join(destination, libraries[0].name);
        fs.mkdirSync(destination);
        fs.copyFileSync(path.join(products, libraries[0].name), library);
        for (const name of ["CMakeCache.txt", "compile_commands.json"])
            fs.copyFileSync(path.join(build, name), path.join(output, `${lane}-${name}`));
        return {
            library,
            metadata: {
                libraryBytes: fs.statSync(library).size,
                librarySha256: fileDigest(library),
                cmakeCacheSha256: fileDigest(path.join(build, "CMakeCache.txt")),
                compileCommandsSha256: fileDigest(path.join(build, "compile_commands.json"))
            }
        };
    } finally {
        // Only the staged DSO survives. Head starts with a new source tree and
        // CMake cache; no compiler or build process overlaps measurement.
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}

export function compareRevisions({
    baseSource,
    baseSha,
    headSource,
    headSha,
    output,
    blocks = 12,
    cc = process.env.CC ?? "clang"
}) {
    for (const sha of [baseSha, headSha]) assert.match(sha, /^[0-9a-f]{40}$/);
    assert.ok(Number.isInteger(blocks) && blocks >= 2 && blocks <= 32);
    const settings = { blocks, warmup: 5, repeats: 9, order: ORDER };
    assert.ok(["linux", "darwin"].includes(os.platform()), "paired runner requires a POSIX host");
    output = path.resolve(output);
    fs.mkdirSync(output, { recursive: true });
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "markdown-core-benchmark-run-"));
    try {
        const driver = path.join(root, "packages/markdown-core/benchmarks/paired_runner.c");
        const executable = path.join(temporary, "paired_runner");
        const input = path.join(temporary, "input.md");
        const bytes = workload();
        fs.writeFileSync(input, bytes);
        // One executable and one public header for both revisions. DSO loading
        // occurs before timing, in a fresh process for each ABBA observation.
        run(cc, [
            "-std=c11",
            "-O3",
            "-DNDEBUG",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            path.join(root, "packages/markdown-core/include"),
            driver,
            "-ldl",
            "-o",
            executable
        ]);
        const lanes = {};
        for (const [lane, repository, sha] of [
            ["base", baseSource, baseSha],
            ["head", headSource, headSha]
        ]) {
            console.log(`Build ${lane} ${sha}`);
            lanes[lane] = buildLibrary(path.resolve(repository), sha, path.join(temporary, lane), output, cc, lane);
            lanes[lane].metadata.runnerSha256 = fileDigest(executable);
        }
        const cpuAffinity =
            os.platform() === "linux"
                ? Number(/^Cpus_allowed_list:\s*(\d+)/m.exec(fs.readFileSync("/proc/self/status", "utf8"))?.[1])
                : null;
        assert.ok(cpuAffinity === null || Number.isSafeInteger(cpuAffinity), "cannot determine permitted CPU affinity");
        const invoke = (lane, mode) => {
            const args = [lanes[lane].library, input, mode, String(settings.warmup), String(settings.repeats)];
            return cpuAffinity === null
                ? run(executable, args)
                : run("taskset", ["-c", String(cpuAffinity), executable, ...args]);
        };
        const baseDump = invoke("base", "dump");
        assert.deepEqual(
            invoke("head", "dump"),
            baseDump,
            "benchmark input produces different canonical output; timing comparison is not equivalent"
        );
        const environment = {
            os: os.platform(),
            arch: os.arch(),
            release: os.release(),
            cpu: os.cpus()[cpuAffinity ?? 0]?.model ?? os.cpus()[0].model,
            logicalCpus: os.cpus().length,
            compiler: run(cc, ["--version"]).toString().split("\n")[0],
            cmake: run("cmake", ["--version"]).toString().split("\n")[0],
            runnerImage: process.env.ImageOS ?? "",
            runnerImageVersion: process.env.ImageVersion ?? "",
            cpuAffinity,
            loadBefore: os.loadavg(),
            loadAfter: []
        };
        const observations = [];
        for (let block = 0; block < blocks; block++) {
            observations.push(
                ORDER.map((lane) => ({
                    lane,
                    ...validateMeasurement(JSON.parse(invoke(lane, "measure").toString()), settings.repeats)
                }))
            );
            console.log(`Measured ABBA block ${block + 1}/${blocks}`);
        }
        environment.loadAfter = os.loadavg();
        const result = validateComparison({
            schema: 2,
            baseSha,
            headSha,
            run: {
                id: Number(process.env.GITHUB_RUN_ID ?? 0),
                attempt: Number(process.env.GITHUB_RUN_ATTEMPT ?? 1),
                job: process.env.GITHUB_JOB ?? "local"
            },
            environment,
            workload: {
                name: "representative_large",
                bytes: bytes.length,
                sha256: digest(bytes),
                canonicalSha256: digest(baseDump)
            },
            harnessSha256: fileDigest(driver),
            settings,
            binaries: Object.fromEntries(Object.entries(lanes).map(([lane, value]) => [lane, value.metadata])),
            blocks: observations
        });
        const encoded = JSON.stringify(result, null, 4) + "\n";
        assert.ok(Buffer.byteLength(encoded) <= MAX_RESULT_BYTES, "result exceeds artifact limit");
        fs.writeFileSync(path.join(output, "comparison.json"), encoded);
        const summary = renderComparison(result);
        fs.writeFileSync(path.join(output, "summary.md"), summary);
        if (process.env.GITHUB_STEP_SUMMARY) fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, summary);
        return result;
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const values = {};
    for (let i = 2; i < process.argv.length; i += 2) {
        assert.ok(
            ["--base-source", "--base-sha", "--head-source", "--head-sha", "--output", "--blocks", "--cc"].includes(
                process.argv[i]
            ) && process.argv[i + 1],
            "invalid benchmark arguments"
        );
        values[process.argv[i].slice(2)] = process.argv[i + 1];
    }
    for (const required of ["base-source", "base-sha", "head-source", "head-sha", "output"])
        assert.ok(values[required], `--${required} is required`);
    compareRevisions({
        baseSource: values["base-source"],
        baseSha: values["base-sha"],
        headSource: values["head-source"],
        headSha: values["head-sha"],
        output: values.output,
        blocks: Number(values.blocks ?? 12),
        cc: values.cc ?? process.env.CC ?? "clang"
    });
}
