import assert from "node:assert/strict";
import { Buffer } from "node:buffer";

export const ARTIFACT = "pr-benchmark-comparison";
export const MAX_RESULT_BYTES = 512 * 1024;
export const ORDER = ["base", "head", "head", "base"];
export const MARKER = "<!-- markdown-core-pr-benchmark -->";

export function workload() {
    return Buffer.from(
        "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n".repeat(2000)
    );
}

function keys(value, names) {
    assert.ok(value && typeof value === "object" && !Array.isArray(value), "expected an object");
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), "unexpected result fields");
}

function integer(value, min, max) {
    assert.ok(Number.isSafeInteger(value) && value >= min && value <= max, "integer outside result bounds");
}

function text(value, max = 2048) {
    assert.ok(typeof value === "string" && value.length <= max, "invalid metadata string");
}

function hash(value, length = 64) {
    assert.ok(typeof value === "string" && new RegExp(`^[0-9a-f]{${length}}$`).test(value), "invalid digest");
}

export function validateMeasurement(value, repeats) {
    keys(value, ["rssKiB", "parseNs", "freeNs"]);
    integer(value.rssKiB, 1, 1e12);
    for (const name of ["parseNs", "freeNs"]) {
        assert.ok(Array.isArray(value[name]) && value[name].length === repeats, "missing raw samples");
        for (const sample of value[name]) integer(sample, 1, 60e9);
    }
    return value;
}

export function validateComparison(value, expected = {}) {
    keys(value, [
        "schema",
        "baseSha",
        "headSha",
        "run",
        "environment",
        "workload",
        "harnessSha256",
        "settings",
        "binaries",
        "blocks"
    ]);
    assert.equal(value.schema, 2, "retired or unknown benchmark schema");
    hash(value.baseSha, 40);
    hash(value.headSha, 40);
    hash(value.harnessSha256);
    keys(value.run, ["id", "attempt", "job"]);
    integer(value.run.id, 0, Number.MAX_SAFE_INTEGER);
    integer(value.run.attempt, 1, 10000);
    assert.ok(["compare", "local"].includes(value.run.job));
    for (const name of ["baseSha", "headSha"]) {
        if (expected[name] !== undefined) assert.equal(value[name], expected[name], `stale ${name}`);
    }
    for (const name of ["id", "attempt"]) {
        if (expected.run?.[name] !== undefined) assert.equal(value.run[name], expected.run[name], `wrong run ${name}`);
    }
    keys(value.workload, ["name", "bytes", "sha256", "canonicalSha256"]);
    assert.equal(value.workload.name, "representative_large");
    integer(value.workload.bytes, 1, 16 * 1024 * 1024);
    hash(value.workload.sha256);
    hash(value.workload.canonicalSha256);
    keys(value.settings, ["blocks", "warmup", "repeats", "order"]);
    integer(value.settings.blocks, 2, 32);
    integer(value.settings.warmup, 1, 32);
    integer(value.settings.repeats, 3, 32);
    assert.deepEqual(value.settings.order, ORDER);
    keys(value.environment, [
        "os",
        "arch",
        "release",
        "cpu",
        "logicalCpus",
        "compiler",
        "cmake",
        "runnerImage",
        "runnerImageVersion",
        "cpuAffinity",
        "loadBefore",
        "loadAfter"
    ]);
    assert.ok(["linux", "darwin"].includes(value.environment.os));
    assert.ok(["x64", "arm64"].includes(value.environment.arch));
    for (const name of ["release", "cpu", "compiler", "cmake", "runnerImage", "runnerImageVersion"])
        text(value.environment[name]);
    integer(value.environment.logicalCpus, 1, 65536);
    if (value.environment.cpuAffinity !== null) integer(value.environment.cpuAffinity, 0, 65535);
    for (const name of ["loadBefore", "loadAfter"]) {
        const load = value.environment[name];
        assert.ok(
            Array.isArray(load) && load.length === 3 && load.every((n) => Number.isFinite(n) && n >= 0 && n <= 1e6)
        );
    }
    keys(value.binaries, ["base", "head"]);
    for (const name of ["base", "head"]) {
        const binary = value.binaries[name];
        keys(binary, ["libraryBytes", "librarySha256", "runnerSha256", "cmakeCacheSha256", "compileCommandsSha256"]);
        integer(binary.libraryBytes, 1, 1024 * 1024 * 1024);
        for (const field of ["librarySha256", "runnerSha256", "cmakeCacheSha256", "compileCommandsSha256"])
            hash(binary[field]);
    }
    assert.equal(
        value.binaries.base.runnerSha256,
        value.binaries.head.runnerSha256,
        "different measurement executables"
    );
    assert.ok(Array.isArray(value.blocks) && value.blocks.length === value.settings.blocks, "incomplete paired run");
    for (const block of value.blocks) {
        assert.ok(Array.isArray(block) && block.length === ORDER.length, "incomplete ABBA block");
        for (let i = 0; i < ORDER.length; i++) {
            const { lane, ...measurement } = block[i];
            assert.equal(lane, ORDER[i], "unbalanced run order");
            validateMeasurement(measurement, value.settings.repeats);
        }
    }
    return value;
}

export function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    const middle = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function mean(values) {
    return values.reduce((a, b) => a + b, 0) / values.length;
}

// Resample whole ABBA blocks, not the correlated samples within a process.
// The interval describes this job's paired observations, not other machines.
export function pairedEstimate(blockLogRatios) {
    let state = 0x2730244;
    const estimates = [];
    for (let repeat = 0; repeat < 10000; repeat++) {
        let sum = 0;
        for (let i = 0; i < blockLogRatios.length; i++) {
            state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
            sum += blockLogRatios[Math.floor((state / 4294967296) * blockLogRatios.length)];
        }
        estimates.push(Math.exp(sum / blockLogRatios.length));
    }
    estimates.sort((a, b) => a - b);
    return { ratio: Math.exp(mean(blockLogRatios)), low: estimates[249], high: estimates[9749] };
}

export function summarize(value) {
    validateComparison(value);
    const phases = {};
    for (const phase of ["parse", "free", "total"]) {
        const samples = (run) => (phase === "total" ? run.parseNs.map((p, i) => p + run.freeNs[i]) : run[`${phase}Ns`]);
        const logs = value.blocks.map((block) => {
            const times = block.map((run) => median(samples(run)));
            return (Math.log(times[1] / times[0]) + Math.log(times[2] / times[3])) / 2;
        });
        phases[phase] = {
            base: median(value.blocks.flatMap((block) => block.filter((run) => run.lane === "base").flatMap(samples))),
            head: median(value.blocks.flatMap((block) => block.filter((run) => run.lane === "head").flatMap(samples))),
            ...pairedEstimate(logs)
        };
    }
    const rss = Object.fromEntries(
        ["base", "head"].map((lane) => [
            lane,
            median(value.blocks.flatMap((block) => block.filter((run) => run.lane === lane).map((run) => run.rssKiB)))
        ])
    );
    return { phases, rss };
}

const percent = (ratio) => `${ratio >= 1 ? "+" : ""}${((ratio - 1) * 100).toFixed(1)}%`;

export function renderComparison(value) {
    const { phases, rss } = summarize(value);
    const lines = [
        MARKER,
        "## PR benchmark",
        "",
        `Same runner: base \`${value.baseSha.slice(0, 12)}\` → head \`${value.headSha.slice(0, 12)}\`.`,
        "",
        `Input: ${value.workload.bytes.toLocaleString("en-US")} B; SHA-256 \`${value.workload.sha256.slice(0, 12)}\`. Canonical output agrees.`,
        `${value.settings.blocks} ABBA blocks; each fresh process has ${value.settings.warmup} warmups and ${value.settings.repeats} timed parses.`,
        "",
        "| Phase | Base median | Head median | Paired change | 95% interval |",
        "| --- | ---: | ---: | ---: | ---: |"
    ];
    for (const [name, phase] of Object.entries(phases)) {
        lines.push(
            `| ${name === "total" ? "parse + free" : name} | ${(phase.base / 1e6).toFixed(3)} ms | ${(phase.head / 1e6).toFixed(3)} ms | ${percent(phase.ratio)} | ${percent(phase.low)} to ${percent(phase.high)} |`
        );
    }
    lines.push(
        "",
        `Median process peak RSS: base ${rss.base.toLocaleString("en-US")} KiB; head ${rss.head.toLocaleString("en-US")} KiB.`,
        `Library size: base ${value.binaries.base.libraryBytes.toLocaleString("en-US")} B; head ${value.binaries.head.libraryBytes.toLocaleString("en-US")} B.`,
        "",
        "Paired change is the geometric mean of within-block ratios. The bootstrap interval resamples complete ABBA blocks; it describes this job, not cross-machine variation.",
        "A range crossing 0% does not resolve the direction in this run. Timings remain informational; no wall-clock pass/fail threshold is applied.",
        "Raw samples, workload/driver/library hashes, and environment metadata are attached to this run. Both lanes are PR-controlled diagnostic data, never reusable trusted baselines."
    );
    return lines.join("\n") + "\n";
}
