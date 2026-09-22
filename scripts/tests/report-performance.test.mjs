import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

import { parseCallgrind } from "../lib/callgrind.mjs";
import { pairReview } from "../lib/pair-review.mjs";
import { referenceFor, render, summarize } from "../report-performance.mjs";

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
