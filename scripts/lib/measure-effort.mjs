import { Buffer } from "node:buffer";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import {
    boundaryFixtures,
    boundaryModel,
    boundaryOperations,
    boundaryPairAudit,
    boundarySplits,
    verifyBoundaryReceipt
} from "./effort-boundaries.mjs";
import { baseName, costRecord, foldNames, parseCallgrind } from "./callgrind.mjs";
import { CACHE, measurementEnvironment, measurementRoot } from "./measurement.mjs";
import { compiledFlags } from "./compile-identity.mjs";

const fail = (message) => {
    throw new Error(message);
};
const digest = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");
const invoke = (command, args, options) => {
    const result = spawnSync(command, args, { encoding: "utf8", maxBuffer: 32 * 1024 * 1024, ...options });
    if (result.error || result.status !== 0)
        fail(`boundary runner failed: ${command}: ${result.error ?? result.stderr}`);
    return result.stdout;
};

export function boundaryIdentity(root) {
    const files = [
        "docs/architecture/benchmark-effort-boundaries.md",
        "docs/architecture/benchmark-parser-effort.md",
        "scripts/lib/effort-boundaries.mjs",
        "scripts/lib/measure-effort.mjs",
        "packages/markdown-core/benchmarks/effort_runner.h",
        "packages/markdown-core/benchmarks/effort_runner.c",
        "packages/markdown-core/benchmarks/effort_adapter.inc",
        "packages/markdown-core/benchmarks/markdown_core_effort.c",
        "packages/markdown-core/benchmarks/cmark_effort.c",
        "packages/markdown-core/benchmarks/CMakeLists.txt"
    ];
    return digest(files.map((file) => `${file}\0${digest(fs.readFileSync(path.join(root, file)))}`).join("\n"));
}

export function readBoundaryEdges(text, iterations) {
    const profile = foldNames(parseCallgrind(text), (name) =>
        baseName(name).replace(/(\.(constprop|isra|part|cold)\.?\d*)+$/u, "")
    );
    return Object.fromEntries(
        ["prepare", "operation", "release"].map((stage) => {
            const edge = [...profile.edges.values()].find(
                (e) => e.caller === "main" && e.callee === `bench_effort_${stage}`
            );
            if (!edge || edge.calls !== (stage === "operation" ? iterations : 1))
                fail(`missing or repeated ${stage} boundary`);
            const cost = costRecord(profile, edge.cost);
            if (!(cost.Ir > 0)) fail(`empty ${stage} measurement`);
            return [stage, { calls: edge.calls, cost }];
        })
    );
}

export function measureEffortBoundaries({ root, binaryDir, out, pairs, toolchain, cmark, runMeasurement = true }) {
    const directory = path.join(out, "effort");
    fs.mkdirSync(path.join(directory, "input"), { recursive: true });
    fs.mkdirSync(path.join(directory, "callgrind"), { recursive: true });
    const isolated = measurementRoot(directory, fail);
    const options = { env: measurementEnvironment(isolated), cwd: isolated };
    const runners = { "markdown-core": "markdown_core_effort_runner", cmark: "cmark_effort_runner" };
    const binaries = Object.fromEntries(
        Object.entries(runners).map(([engine, target]) => {
            const file = path.join(binaryDir, "packages/markdown-core/benchmarks", target);
            return [
                engine,
                { file, sha256: digest(fs.readFileSync(file)), compiled: compiledFlags(root, binaryDir, target, fail) }
            ];
        })
    );
    const fixtures = boundaryFixtures();
    // Invalid promises must be rejected by BOTH native runners as well as JS.
    let rejected = 0;
    for (const operation of ["trim", "whitespace", "code", "closer"]) {
        for (const byte of ["trim", "whitespace"].includes(operation) ? [11, 12] : [0, 13]) {
            const file = path.join(directory, "input", `rejected-${operation}-${byte}.bin`);
            fs.writeFileSync(file, Buffer.from([byte]));
            for (const binary of Object.values(binaries)) {
                const result = spawnSync(binary.file, [operation, file, "1", "0", "1"], {
                    ...options,
                    encoding: "utf8"
                });
                if (result.status !== 2 || result.stdout) fail("native admission accepted an invalid boundary promise");
            }
            rejected++;
        }
    }
    const cases = [];
    for (const fixture of fixtures) {
        const input = path.join(directory, "input", `${fixture.id}.bin`);
        fs.writeFileSync(input, fixture.input);
        const engines = {};
        for (const [engine, binary] of Object.entries(binaries)) {
            const args = [fixture.operation, input, "1", String(fixture.start), String(fixture.ticks)];
            verifyBoundaryReceipt(fixture, invoke(binary.file, args, options), 1);
            if (!runMeasurement || !fixture.measure) continue;
            args[2] = "16";
            const dump = path.join(directory, "callgrind", `${engine}.${fixture.id}.out`);
            const stdout = invoke(
                "valgrind",
                [
                    "--tool=callgrind",
                    "--cache-sim=yes",
                    "--dump-instr=no",
                    "--separate-callers=1",
                    ...CACHE,
                    `--callgrind-out-file=${dump}`,
                    "--quiet",
                    binary.file,
                    ...args
                ],
                options
            );
            verifyBoundaryReceipt(fixture, stdout, 16);
            engines[engine] = readBoundaryEdges(fs.readFileSync(dump, "utf8"), 16);
        }
        if (fixture.measure)
            cases.push({
                id: fixture.id,
                operation: fixture.operation,
                bytes: fixture.input.length,
                input: path.relative(directory, input),
                sha256: digest(fixture.input),
                start: fixture.start,
                ticks: fixture.ticks,
                ...(fixture.split ? { split: fixture.split, extent: fixture.extent } : {}),
                engines
            });
    }
    const report = {
        schemaVersion: 1,
        model: boundaryModel,
        scope: "local-operation-including-native-adapters",
        identity: boundaryIdentity(root),
        revision: invoke("git", ["rev-parse", "HEAD"], { cwd: root }).trim(),
        toolchain,
        cmark,
        binaries,
        certificates: boundaryOperations,
        checkedFixtures: fixtures.length,
        rejectedFixtures: rejected,
        splits: boundarySplits().map((split) => ({
            id: split.id,
            operation: split.operation,
            sourceHex: split.input.toString("hex"),
            residual: split.residual,
            segments: split.segments.map((segment) => ({ ...segment, input: segment.input.toString("hex") }))
        })),
        iterations: 16,
        measured: runMeasurement,
        fullParserCertificates: 0,
        pairs: boundaryPairAudit(pairs),
        cases
    };
    fs.writeFileSync(path.join(directory, "effort.json"), JSON.stringify(report, null, 4) + "\n");
    fs.writeFileSync(path.join(directory, "effort.md"), boundaryMarkdown(report));
    return report;
}

export function boundaryMarkdown(report) {
    const lines = [
        "# Equal-optimum local boundary comparison",
        "",
        `Contract identity: ${report.identity}. ${report.checkedFixtures} fixtures checked against an independent byte/state oracle in both engines.`,
        "",
        "Six identical local problems have equal optimal effort under the declared model. Measured Ir compares production code plus native adapters, NOT either implementation with that optimum. Full-parser certificates: 0.",
        "",
        "Preparation and release are measured separately. Their sum with operation is a harness lifecycle, not a decomposed whole-document parse. Existing A/R ratios remain descriptive.",
        "",
        "| Boundary input | Core operation Ir | cmark operation Ir | Core/cmark | Core prepare/release | cmark prepare/release |",
        "| --- | ---: | ---: | ---: | ---: | ---: |"
    ];
    for (const row of report.cases) {
        const a = row.engines["markdown-core"],
            b = row.engines.cmark;
        if (!a || !b) continue;
        lines.push(
            `| ${row.id} | ${a.operation.cost.Ir} | ${b.operation.cost.Ir} | ${(a.operation.cost.Ir / b.operation.cost.Ir).toFixed(3)}x | ${a.prepare.cost.Ir}/${a.release.cost.Ir} | ${b.prepare.cost.Ir}/${b.release.cost.Ir} |`
        );
    }
    lines.push(
        "",
        "## Exhaustive disposition of existing structural proofs",
        "",
        "All rows retain unproved full-parser effort status. The six local contracts above are separate admitted problems, not coverage claims for these parses.",
        "",
        "| Structural proof | Unmatched recognition/construction work |",
        "| --- | --- |"
    );
    for (const pair of report.pairs) lines.push(`| ${pair.proof} | ${pair.local.residual} |`);
    return lines.join("\n") + "\n";
}
