import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { parseCallgrind } from "../lib/callgrind.mjs";
import { pairReview } from "../lib/pair-review.mjs";
import { referenceFor, render, summarize, summarizeArtifact } from "../report-performance.mjs";

test("the reference cohort excludes unmatched fields and selects the declared grammar", () => {
    assert.equal(referenceFor({ dialect: "commonmark", carries: [] }), "cmark");
    assert.equal(referenceFor({ dialect: "extended", gfm: true, carries: [] }), "cmark-gfm");
    assert.equal(referenceFor({ dialect: "commonmark", gfm: true, carries: ["anchor"] }), null);
    assert.equal(referenceFor({ dialect: "extended", carries: [] }), null);
});

const measurement = {
    stages: { source_to_buffer: { cost: { Ir: 30 } }, buffer_to_ast: { cost: { Ir: 50 } } },
    parsePathIr: 90,
    outsideStagesIr: 10
};
const profileText = `events: Ir
fl=node.c
fn=parse
1 70
cfi=malloc.c
cfn=malloc
calls=1 1
1 20
fl=malloc.c
fn=malloc
1 20
fl=harness.c
fn=main
1 10
summary: 102
totals: 100
`;

test("parse-edge accounting stays separate from program self and filtered cohorts", () => {
    const cases = [
        { case: "common", dialect: "commonmark", carries: [] },
        { case: "anchored", dialect: "commonmark", carries: ["anchor"] },
        { case: "dialect", dialect: "extended", carries: [] }
    ].map((entry) => ({ ...entry, scale: 1, engines: { "markdown-core": measurement, cmark: measurement } }));
    const reads = [];
    const summary = summarize({ cases, pairs: [] }, (entry, engine) => {
        reads.push(`${entry.case}:${engine}`);
        return parseCallgrind(profileText);
    });
    assert.equal(summary.full.documents, 3);
    assert.equal(summary.core.documents, 1);
    assert.equal(summary.reference.documents, 1);
    assert.deepEqual(summary.excluded, [{ case: "anchored", scale: 1, carries: ["anchor"] }]);
    assert.equal(summary.core.parse, 90);
    assert.equal(summary.core.program, 100);
    assert.equal(summary.core.allocator, 20);
    assert.equal(summary.core.files["node.c"], 70);
    assert.equal(summary.core.edges["parse → malloc"].ir, 20);
    assert.deepEqual(reads, [
        "common:markdown-core",
        "common:cmark",
        "anchored:markdown-core",
        "dialect:markdown-core"
    ]);
});

test("inconsistent stage accounting and incomplete profile partitions fail closed", () => {
    const entry = {
        case: "common",
        dialect: "commonmark",
        carries: [],
        scale: 1,
        engines: { "markdown-core": measurement, cmark: measurement }
    };
    const report = { cases: [entry], pairs: [] };
    assert.throws(() => summarize(report, () => parseCallgrind(profileText.replace("totals: 100", "totals: 101"))));
    assert.throws(() =>
        summarize(
            { cases: [{ ...entry, engines: { "markdown-core": { ...measurement, outsideStagesIr: 0 } } }], pairs: [] },
            () => parseCallgrind(profileText)
        )
    );
});

const registry = JSON.parse(
    fs.readFileSync(new URL("../../packages/markdown-core/benchmarks/corpus.json", import.meta.url))
);
const subsetCase = (name, dialect = "extended") => ({
    case: name,
    dialect,
    carries: [],
    scale: 1,
    units: 2,
    bytes: 16,
    engines: { "markdown-core": measurement, cmark: measurement }
});
const summarizeSubset = (cases) =>
    summarize({ corpus: { digest: "test" }, cases, pairs: registry.pairs }, () => parseCallgrind(profileText));

test("a selected proof pair ignores unmeasured contracts in the full manifest", () => {
    const pair = registry.pairs.find((pair) => pair.contract.proof === "insertion-strong-v1");
    const cases = [subsetCase(pair.case), subsetCase(pair.isomorph, "commonmark")];
    const summary = summarizeSubset(cases);
    assert.equal(summary.pairs.length, 1);
    assert.equal(summary.pairs[0].case, pair.case);
    assert.equal(summary.pairs[0].sameJob, 1);
    assert.deepEqual(summary.boundaries, []);
    const oneSide = summarizeSubset(cases.slice(0, 1));
    assert.deepEqual(oneSide.pairs, []);
    assert.equal(oneSide.core.documents, 0);
    assert.doesNotMatch(render(oneSide), /NaN|Infinity/u);
});

test("a selected boundary needs its own two measured documents, not its historical counterpart", () => {
    const pair = registry.pairs.find((pair) => pair.contract.review && pairReview(pair).baseline);
    const baseline = pairReview(pair).baseline;
    const without = subsetCase(baseline);
    without.engines["markdown-core"] = {
        ...measurement,
        stages: { ...measurement.stages, buffer_to_ast: { cost: { Ir: 60 } } },
        parsePathIr: 100
    };
    const cases = [subsetCase(pair.case), without];
    const summary = summarizeSubset(cases);
    assert.deepEqual(summary.pairs, []);
    assert.equal(summary.boundaries.length, 1);
    assert.equal(summary.boundaries[0].baseline, baseline);
    assert.equal(summary.boundaries[0].delta, -10);
    assert.deepEqual(summarizeSubset(cases.slice(0, 1)).boundaries, []);
    assert.deepEqual(summarizeSubset(cases.slice(1)).boundaries, []);
});

test("same-job baselines use their own Core profiles and the validated shared references", (t) => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "performance-artifact-"));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    for (const subdir of ["callgrind", "baseline/callgrind"])
        fs.mkdirSync(path.join(directory, subdir), { recursive: true });
    const current = {
        corpus: { digest: "same bytes" },
        pairingDigest: "same proofs",
        pairs: [],
        profile: { flags: "same flags" },
        toolchain: { compiler: "same compiler", compiled: { objects: { cmark: "pinned objects" } } },
        binaries: { cmark: "pinned binary" },
        cmark: { version: "pinned" },
        sourceBudget: { baseline: "base revision" },
        cases: [{ ...subsetCase("common", "commonmark"), sha256: "same document", file: "corpus/common.md" }]
    };
    const baseline = globalThis.structuredClone(current);
    delete baseline.sourceBudget;
    baseline.revision = current.sourceBudget.baseline;
    baseline.cases[0].file = "../corpus/common.md";
    baseline.cases[0].engines["markdown-core"] = {
        stages: { source_to_buffer: { cost: { Ir: 60 } }, buffer_to_ast: { cost: { Ir: 100 } } },
        parsePathIr: 180,
        outsideStagesIr: 20
    };
    const writeReports = () => {
        fs.writeFileSync(path.join(directory, "stages.json"), JSON.stringify(current));
        fs.writeFileSync(path.join(directory, "baseline/stages.json"), JSON.stringify(baseline));
    };
    writeReports();
    for (const engine of ["markdown-core", "cmark"])
        fs.writeFileSync(path.join(directory, "callgrind", `${engine}.common.x1.out`), profileText);
    const beforeProfile = profileText
        .replace(/^1 (\d+)$/gmu, (_, cost) => `1 ${Number(cost) * 2}`)
        .replace("totals: 100", "totals: 200");
    const coreProfile = path.join(directory, "baseline/callgrind/markdown-core.common.x1.out");
    fs.writeFileSync(coreProfile, beforeProfile);
    assert.equal(summarizeArtifact(directory).core.parse, 90);
    const result = summarizeArtifact(directory, true);
    assert.equal(result.core.parse, 180);
    assert.equal(result.core.program, 200);
    assert.equal(result.reference.parse, 90);
    assert.equal(result.reference.program, 100);

    baseline.cases[0].engines.cmark.parsePathIr++;
    writeReports();
    assert.throws(() => summarizeArtifact(directory, true), /shared reference measurements/u);
    baseline.cases[0].engines.cmark.parsePathIr--;
    baseline.binaries.cmark = "another binary";
    writeReports();
    assert.throws(() => summarizeArtifact(directory, true), /cmark binary/u);
    baseline.binaries.cmark = current.binaries.cmark;
    baseline.cases[0].sha256 = "different bytes";
    writeReports();
    assert.throws(() => summarizeArtifact(directory, true), /baseline document identity/u);
    baseline.cases[0].sha256 = current.cases[0].sha256;
    writeReports();
    fs.unlinkSync(coreProfile);
    assert.throws(() => summarizeArtifact(directory, true), /ENOENT/u);
});
