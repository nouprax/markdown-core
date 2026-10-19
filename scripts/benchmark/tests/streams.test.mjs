import assert from "node:assert/strict";
import test from "node:test";

import { markdownReport, mergeParts, regressed } from "../streams.mjs";

/* A set of `count` workloads measured in `shards` parts: workload k is the
 * (k / shards)th result of part k mod shards. */
const parts = (count, shards) =>
    Array.from({ length: shards }, (_, index) => ({
        schemaVersion: 4,
        subjects: ["reparse", "session", "binding"],
        toolchain: { compiler: "cc" },
        binaries: { "markdown-core": { sha256: "a" }, "markdown-core streams": { sha256: "b" } },
        profile: { compiler: "cc", flags: "-O2" },
        workloads: { version: "v2", set: "corpus", digest: "d".repeat(64), count },
        baseline: { revision: "e".repeat(40), binaries: { "markdown-core": { sha256: "c" } } },
        shard: { index, count: shards },
        results: Array.from({ length: count }, (_, position) => ({ name: `w${position}` })).filter(
            (_, position) => position % shards === index
        )
    }));

test("merged parts hold every workload once, in the set's order", () => {
    const merged = mergeParts(parts(10, 3).reverse());
    assert.deepEqual(
        merged.results.map((row) => row.name),
        Array.from({ length: 10 }, (_, position) => `w${position}`)
    );
    assert.equal(merged.shard, undefined);
    assert.equal(merged.workloads.count, 10);
});

test("a missing, repeated or foreign part is refused", () => {
    assert.throws(() => mergeParts(parts(10, 3).slice(1)), /2 of 3 parts/);
    const repeated = parts(10, 3);
    repeated[2] = repeated[1];
    assert.throws(() => mergeParts(repeated), /not one set of shards/);
    const foreign = parts(10, 3);
    foreign[1].binaries = { ...foreign[1].binaries, "markdown-core": { sha256: "c" } };
    assert.throws(() => mergeParts(foreign), /differ in binaries/);
    const rebased = parts(10, 3);
    rebased[2].baseline = { ...rebased[2].baseline, revision: "f".repeat(40) };
    assert.throws(() => mergeParts(rebased), /differ in baseline/);
    const short = parts(10, 3);
    short[0].results.pop();
    assert.throws(() => mergeParts(short), /no part holds workload/);
    const long = parts(10, 3);
    long[0].results.push({ name: "extra" });
    assert.throws(() => mergeParts(long), /more results/);
});

/* A measured workload of `windows` windows of `ir` each, for R and S, with
 * the base's figures scaled by `base`, and B a fifth of S. */
const row = (name, ir, base) => {
    const windows = [ir, ir];
    const figures = { p50: ir, p95: ir, max: ir, total: 2 * ir };
    const scaled = Object.fromEntries(Object.entries(figures).map(([key, value]) => [key, value / base]));
    return {
        name,
        source: "grammar corpus",
        family: "tokens",
        size: null,
        windows,
        reparse: { ir: figures },
        session: { windows, ir: figures },
        binding: { windows: windows.map((cost) => cost / 5), ir: figures },
        neverWorse: [],
        oneshot: { ir: 3 * ir },
        base: { reparse: scaled, session: scaled },
        regressions:
            base > 1.02
                ? ["reparse", "session"].flatMap((subject) =>
                      ["p95", "max", "total"].map((metric) => ({ subject, metric, ratio: base }))
                  )
                : []
    };
};

test("6.4 lists every figure above the base, and the report says how many workloads passed", () => {
    const report = { ...parts(2, 1)[0], shard: undefined, results: [row("w0", 100, 1.01), row("w1", 100, 1.05)] };
    const worse = regressed(report);
    assert.equal(worse.length, 6);
    assert.ok(worse.every((entry) => entry.name === "w1"));
    const rendered = markdownReport(report);
    assert.match(rendered, /## Regression gate/);
    assert.match(rendered, /1\/2 workloads passed/);
    assert.match(rendered, /- w1 session total: 1\.0500× base/);
    assert.doesNotMatch(markdownReport({ ...report, baseline: null }), /Regression gate/);
});

test("the report pools B beside S, without a gate", () => {
    const report = { ...parts(2, 1)[0], shard: undefined, results: [row("w0", 100, 1), row("w1", 300, 1)] };
    const rendered = markdownReport(report);
    assert.match(rendered, /## Bindings' C side/);
    assert.match(rendered, /\| grammar corpus \| tokens \| {2}\| 20 \| 60 \| 60 \| 160 \| 0\.200× \|/);
    assert.equal(regressed(report).length, 0);
});
