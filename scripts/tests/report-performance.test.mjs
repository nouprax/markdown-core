import assert from "node:assert/strict";
import test from "node:test";

import { parseCallgrind } from "../lib/callgrind.mjs";
import { referenceFor, summarize } from "../report-performance.mjs";

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
