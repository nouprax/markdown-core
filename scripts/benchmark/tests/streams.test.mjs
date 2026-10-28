import assert from "node:assert/strict";
import test from "node:test";

import { markdownReport, mergeParts, regressed, steep, violations } from "../streams.mjs";

/* A set of `count` stream workloads and `edits` edit workloads measured in
 * `shards` parts: workload k of each is the (k / shards)th result of part
 * k mod shards. */
const parts = (count, shards, edits = 0) =>
    Array.from({ length: shards }, (_, index) => ({
        schemaVersion: 7,
        subjects: ["reparse", "session", "binding"],
        editSubjects: ["reparse"],
        toolchain: { compiler: "cc" },
        binaries: { "markdown-core": { sha256: "a" }, "markdown-core streams": { sha256: "b" } },
        profile: { compiler: "cc", flags: "-O2" },
        workloads: { version: "v2", set: "corpus", digest: "d".repeat(64), count, edits },
        baseline: { revision: "e".repeat(40), binaries: { "markdown-core": { sha256: "c" } } },
        shard: { index, count: shards },
        results: Array.from({ length: count }, (_, position) => ({ name: `w${position}` })).filter(
            (_, position) => position % shards === index
        ),
        edits: Array.from({ length: edits }, (_, position) => ({ name: `e${position}`, scripts: [] })).filter(
            (_, position) => position % shards === index
        )
    }));

test("merged parts hold every workload once, in the set's order", () => {
    const merged = mergeParts(parts(10, 3, 4).reverse());
    assert.deepEqual(
        merged.results.map((row) => row.name),
        Array.from({ length: 10 }, (_, position) => `w${position}`)
    );
    assert.deepEqual(
        merged.edits.map((row) => row.name),
        ["e0", "e1", "e2", "e3"]
    );
    assert.equal(merged.shard, undefined);
    assert.equal(merged.workloads.count, 10);
});

test("a part short of or past its edit workloads is refused", () => {
    const short = parts(10, 3, 4);
    short[1].edits.pop();
    assert.throws(() => mergeParts(short), /no part holds edit workload 1/);
    const long = parts(10, 3, 4);
    long[2].edits.push({ name: "extra", scripts: [] });
    assert.throws(() => mergeParts(long), /more edits/);
});

test("the report pools each edit family's windows by documents and size, without a gate", () => {
    const report = {
        ...parts(1, 1)[0],
        shard: undefined,
        baseline: null,
        results: [row("w0", 100, 1)],
        edits: [
            {
                name: "a.edits",
                source: "grammar corpus",
                size: null,
                scripts: [
                    { name: "typing-word", family: "typing", edits: 3, windows: [10, 20, 30] },
                    { name: "far", family: "far", edits: 1, windows: [40] }
                ]
            },
            {
                name: "b.edits",
                source: "grammar corpus",
                size: null,
                scripts: [{ name: "typing-word", family: "typing", edits: 1, windows: [50] }]
            },
            {
                name: "prose-ascii-64k.edits",
                source: "prose",
                size: 65536,
                scripts: [{ name: "far", family: "far", edits: 64, windows: [7, 8, 9, 10] }]
            }
        ]
    };
    const rendered = markdownReport(report);
    assert.match(rendered, /## Edits/);
    assert.match(rendered, /\| grammar corpus \| typing \| {2}\| 2 \| 4 \| 4 \| 20 \| 50 \| 50 \| 110 \|/);
    assert.match(rendered, /\| grammar corpus \| far \| {2}\| 1 \| 1 \| 1 \| 40 \| 40 \| 40 \| 40 \|/);
    assert.match(rendered, /\| prose \| far \| 65,536 \| 1 \| 64 \| 4 \| 8 \| 10 \| 10 \| 34 \|/);
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
        local: false,
        steps: 1,
        windows,
        reparse: { ir: figures },
        session: { windows, ir: figures },
        binding: { windows: windows.map((cost) => cost / 5), ir: figures },
        flatness: [],
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

test("6.3 lists every workload above 1.25 times reparse in total, beside S / R in total and on its highest step", () => {
    const over = {
        ...row("w1", 100, 1),
        session: { windows: [300, 10], ir: { p50: 10, p95: 300, max: 300, total: 310 } },
        neverWorse: [{ ratio: 1.55 }]
    };
    const report = { ...parts(2, 1)[0], shard: undefined, results: [row("w0", 100, 1), over] };
    assert.deepEqual(violations(report), [{ name: "w1", ratio: 1.55 }]);
    const rendered = markdownReport(report);
    assert.match(
        rendered,
        /\| grammar corpus \| tokens \| {2}\| 2 \| 4 \| 100 \| 100 \| 100 \| 400 \| 100 \| 300 \| 300 \| 510 \| 1\.275× \| 3\.000× \| 600 \|/
    );
    assert.match(rendered, /6\.3 fails on 1 workload\(s\):/);
    assert.match(rendered, /- w1: 1\.550× reparse in total/);
    assert.equal(violations({ ...report, results: [row("w0", 100, 1)] }).length, 0);
});

test("6.2 lists every point above 1.25 times the first sixteenth, and the report names them", () => {
    /* A long stream of a local shape: S without B, and outside S / R. */
    const long = (name, flatness) => ({
        ...row(name, 100, 1),
        source: "list",
        family: "rows",
        size: 16384,
        local: true,
        steps: 2048,
        binding: null,
        neverWorse: [],
        flatness
    });
    const report = {
        ...parts(2, 1)[0],
        shard: undefined,
        baseline: null,
        results: [long("list-ascii-16k.rows", []), long("quote-ascii-16k.rows", [{ point: 16384, ratio: 1.31 }])]
    };
    assert.deepEqual(steep(report), [{ name: "quote-ascii-16k.rows", point: 16384, ratio: 1.31 }]);
    const rendered = markdownReport(report);
    assert.match(
        rendered,
        /6\.2 fails at 1 point\(s\):\n\n- quote-ascii-16k\.rows byte 16,384: 1\.310× the first sixteenth/
    );
    assert.match(
        rendered,
        /\| list \| rows \| 16,384 \| 2 \| 4 \| 100 \| 100 \| 100 \| 400 \| 100 \| 100 \| 100 \| 400 \| {2}\| {2}\| 600 \|/
    );
    assert.doesNotMatch(markdownReport({ ...report, results: [long("list-ascii-16k.rows", [])] }), /6\.2 fails/);
});

test("6.3 measures a stepwise stream however long it is, and S / R covers it", () => {
    /* A shape 6.2 leaves to 6.3, at its smallest size: R and S at every step,
     * without B. */
    const steps = 2048;
    const windows = Array.from({ length: steps + 1 }, () => 100);
    const session = windows.map((cost, window) => (window === steps ? 400 : cost));
    const stepwise = {
        ...row("unclosed-grid-ascii-16k.rows", 100, 1),
        source: "unclosed-grid",
        family: "rows",
        size: 16384,
        steps,
        windows,
        session: { windows: session, ir: { p50: 100, p95: 100, max: 400, total: 100 * steps + 400 } },
        binding: null,
        neverWorse: []
    };
    const report = { ...parts(1, 1)[0], shard: undefined, baseline: null, results: [stepwise] };
    const ratio = (100 * steps + 400) / (100 * (steps + 1));
    assert.match(
        markdownReport(report),
        new RegExp(
            `\\| unclosed-grid \\| rows \\| 16,384 \\| 1 \\| 2,049 \\| .* \\| ${ratio.toFixed(3)}× \\| 4\\.000× \\|`,
            "u"
        )
    );
});
