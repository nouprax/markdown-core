#!/usr/bin/env node

import { lstat, mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import path from "node:path";

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index];
    const value = process.argv[index + 1];
    if (!key?.startsWith("--") || value === undefined) {
        throw new Error("usage: collect-pr-metrics.mjs --platform NAME --logs DIR --output FILE");
    }
    args.set(key.slice(2), value);
}

const platform = args.get("platform");
const logsDirectory = args.get("logs");
const output = args.get("output");
if (!platform || !logsDirectory || !output) {
    throw new Error("--platform, --logs, and --output are required");
}
if (!new Set(["linux", "macos"]).has(platform)) {
    throw new Error(`unsupported platform: ${platform}`);
}

const allowedRuntimes = new Set(["c", "swift", "kotlin", "es"]);
const allowedWorkloads = new Set(["representative_large", "large_document", "deep_nesting", "spec_corpus"]);
const allowedBoundaries = new Map([
    ["c", new Set(["native_parse", "native_feed_loop"])],
    ["swift", new Set(["native_feed_seal_and_value_copy", "native_feed_loop"])],
    ["kotlin", new Set(["jni_feed_seal_and_value_copy", "jni_feed_loop"])],
    ["es", new Set(["wasm_feed_seal_and_value_copy", "wasm_feed_loop"])]
]);

async function filesBelow(root) {
    const files = [];
    async function visit(current) {
        let entries;
        try {
            entries = await readdir(current, { withFileTypes: true });
        } catch {
            return;
        }
        for (const entry of entries) {
            const entryPath = path.join(current, entry.name);
            if (entry.isDirectory()) {
                await visit(entryPath);
            } else if (entry.isFile()) {
                files.push(entryPath);
            }
        }
    }
    await visit(root);
    return files;
}

const benchmarks = [];
const logFiles = await filesBelow(logsDirectory);
for (const logFile of logFiles.sort()) {
    const contents = await readFile(logFile, "utf8");
    for (const line of contents.split(/\r?\n/u)) {
        if (!line.includes("runtime=") || !line.includes("median_ns=")) continue;
        const fields = new Map();
        for (const token of line.split(/\s+/u)) {
            const separator = token.indexOf("=");
            if (separator > 0) fields.set(token.slice(0, separator), token.slice(separator + 1));
        }
        const runtime = fields.get("runtime");
        const workload = fields.get("workload");
        const boundary = fields.get("boundary");
        const workloadVersion = Number(fields.get("workload_version"));
        const bytes = Number(fields.get("bytes"));
        const warmup = Number(fields.get("warmup"));
        const repeats = Number(fields.get("repeats"));
        const medianNs = Number(fields.get("median_ns"));
        const depthText = fields.get("depth");
        const commitsText = fields.get("commits");
        const depth = depthText === undefined ? null : Number(depthText);
        const commits = commitsText === undefined ? null : Number(commitsText);
        if (
            !allowedRuntimes.has(runtime) ||
            !allowedWorkloads.has(workload) ||
            !allowedBoundaries.get(runtime)?.has(boundary) ||
            !Number.isSafeInteger(workloadVersion) ||
            workloadVersion <= 0 ||
            !Number.isSafeInteger(bytes) ||
            bytes <= 0 ||
            !Number.isSafeInteger(warmup) ||
            warmup < 0 ||
            !Number.isSafeInteger(repeats) ||
            repeats <= 0 ||
            !(depth === null || (Number.isSafeInteger(depth) && depth > 0)) ||
            !(commits === null || (Number.isSafeInteger(commits) && commits > 0)) ||
            !Number.isSafeInteger(medianNs) ||
            medianNs < 0
        ) {
            continue;
        }
        const memoryText = fields.get("peak_rss_kib") ?? fields.get("rss_kib");
        const memoryKiB = Number(memoryText);
        benchmarks.push({
            runtime,
            workload,
            boundary,
            workloadVersion,
            bytes,
            depth,
            commits,
            warmup,
            repeats,
            medianNs,
            memoryKiB: Number.isSafeInteger(memoryKiB) && memoryKiB >= 0 ? memoryKiB : null
        });
    }
}

const sizeDefinitions =
    platform === "linux"
        ? [
              {
                  name: "c-shared-library",
                  root: "build/cmake",
                  matches: (file) => /libmarkdown-core\.so(?:\.\d+)*$/u.test(file)
              },
              {
                  name: "kotlin-jvm-jar",
                  root: "packages/kotlin-markdown-core/build",
                  matches: (file) =>
                      /kotlin-markdown-core-jvm-[^/]+\.jar$/u.test(file) &&
                      !/(?:sources|javadoc|metadata)\.jar$/u.test(file)
              },
              {
                  name: "es-wasm",
                  root: "packages/es-markdown-core/dist",
                  matches: (file) => file.endsWith("/markdown-core.wasm")
              }
          ]
        : [];

const sizes = [];
for (const definition of sizeDefinitions) {
    const candidates = (await filesBelow(definition.root)).filter(definition.matches);
    let largest = null;
    for (const candidate of candidates) {
        const candidateLstat = await lstat(candidate);
        if (candidateLstat.isSymbolicLink()) continue;
        const candidateStat = await stat(candidate);
        if (!largest || candidateStat.size > largest.bytes) {
            largest = { name: definition.name, bytes: candidateStat.size };
        }
    }
    if (largest) sizes.push(largest);
}

const uniqueBenchmarks = [
    ...new Map(
        benchmarks.map((benchmark) => [`${benchmark.runtime}:${benchmark.boundary}:${benchmark.workload}`, benchmark])
    ).values()
].sort((left, right) =>
    `${left.runtime}:${left.boundary}:${left.workload}`.localeCompare(
        `${right.runtime}:${right.boundary}:${right.workload}`
    )
);

await mkdir(path.dirname(output), { recursive: true });
await writeFile(
    output,
    `${JSON.stringify(
        {
            schema: 2,
            sourceSha: process.env.SOURCE_SHA ?? process.env.GITHUB_SHA ?? null,
            platform,
            benchmarks: uniqueBenchmarks,
            sizes
        },
        null,
        2
    )}\n`
);
