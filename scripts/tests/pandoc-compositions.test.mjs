import assert from "node:assert/strict";
import fs from "node:fs";
import { test } from "node:test";
import {
    compositionCases,
    compositionEvidenceCases,
    compositionFuzzCases,
    compareCompositions
} from "../lib/pandoc-compositions.mjs";
import { source } from "../lib/pandoc-oracle.mjs";

const read = (name) => JSON.parse(fs.readFileSync(new URL(`../../specs/oracles/pandoc/${name}.json`, import.meta.url)));
const corpus = read("corpus");
const seeds = read("composition-seeds");

test("reviewed seeds exercise every selected feature in both adjacency orders", () => {
    const cases = compositionCases(source, corpus, seeds);
    const features = Object.keys(source.profileBoundary.publicToPandocExtensions);
    assert.equal(cases.length, features.length * (features.length - 1));
    assert.deepEqual(cases, compositionCases(source, corpus, { ...seeds, features: [...seeds.features].reverse() }));
    for (const first of features) {
        for (const second of features) {
            if (first === second) continue;
            const testCase = cases.find((value) => value.features[0] === first && value.features[1] === second);
            assert.ok(testCase, `${first} then ${second}`);
            const [left, right] = testCase.seeds.map((id) => corpus.cases.find((seed) => seed.id === id));
            assert.equal(testCase.input, `${left.input.replace(/\n*$/, "")}\n\n${right.input.replace(/\n*$/, "")}\n`);
            assert.deepEqual(
                new Set(testCase.from.split("+").slice(1)),
                new Set([...left.from.split("+").slice(1), ...right.from.split("+").slice(1)])
            );
        }
    }
});

test("seed inventory rejects omissions, stale IDs, duplicate witnesses and uncontrolled readers", () => {
    for (const badSeeds of [
        { ...seeds, schemaVersion: 2 },
        { ...seeds, features: seeds.features.slice(1) },
        { ...seeds, features: [...seeds.features, seeds.features[0]] },
        { ...seeds, features: seeds.features.map((seed, index) => (index ? seed : { ...seed, case: "missing" })) },
        { ...seeds, features: seeds.features.map((seed) => ({ ...seed, case: seeds.features[0].case })) }
    ])
        assert.throws(() => compositionCases(source, corpus, badSeeds));
    const id = seeds.features[0].case;
    for (const from of [
        "markdown",
        "markdown+auto_identifiers",
        "markdown_strict-auto_identifiers",
        "markdown_strict+unknown",
        "markdown_strict+auto_identifiers+auto_identifiers"
    ]) {
        const changed = {
            ...corpus,
            cases: corpus.cases.map((value) => (value.id === id ? { ...value, from } : value))
        };
        assert.throws(() => compositionCases(source, changed, seeds));
    }
    assert.throws(() => compositionCases(source, { ...corpus, cases: [...corpus.cases, corpus.cases[0]] }, seeds));
});

test("composition results keep the actual trees and never inherit a seed waiver", () => {
    const cases = compositionCases(source, corpus, seeds).slice(0, 2);
    const oracle = (input) => ({ kind: "Document", children: [{ kind: "Text", literal: input }] });
    const product = (input) => ({ kind: "Document", children: [{ kind: "Code", literal: input }] });
    const agreement = compareCompositions(cases, oracle, oracle);
    assert.equal(agreement.agreements, 2);
    assert.equal(agreement.differences, 0);
    const report = compareCompositions(cases, oracle, product);
    assert.equal(report.agreements, 0);
    assert.equal(report.differences, 2);
    assert.deepEqual(report.cases[0].oracle, oracle(cases[0].input));
    assert.deepEqual(report.cases[0].core, product(cases[0].input));
    assert.notEqual(report.cases[0].oracleDigest, report.cases[0].coreDigest);
    assert.deepEqual(report, compareCompositions(cases, oracle, product));
    assert.throws(() => compareCompositions([cases[0], cases[0]], oracle, product), /duplicate/);
    assert.throws(
        () =>
            compareCompositions(
                cases,
                () => {
                    throw new Error("oracle failed");
                },
                product
            ),
        /oracle failed/
    );
    assert.throws(
        () =>
            compareCompositions(cases, oracle, () => {
                throw new Error("product failed");
            }),
        /product failed/
    );
});

test("nested and opaque witnesses cover all features without reusing adjacency waivers", () => {
    const cases = compositionEvidenceCases(source, corpus, seeds);
    assert.equal(cases.length, 306 + 3 * 18);
    assert.equal(new Set(cases.map((value) => value.id)).size, cases.length);
    for (const context of ["blockquote", "grid-cell", "code-block"]) {
        assert.equal(cases.filter((value) => value.context === context).length, 18);
    }
    assert.deepEqual(
        cases,
        compositionEvidenceCases(source, corpus, { ...seeds, features: [...seeds.features].reverse() })
    );
});

test("mutation corpus is deterministic and exercises every requested marker family", () => {
    const cases = compositionFuzzCases(source, corpus, seeds);
    assert.equal(cases.length, 128);
    assert.deepEqual(
        cases,
        compositionFuzzCases(source, corpus, { ...seeds, features: [...seeds.features].reverse() })
    );
    const inputs = cases.map((value) => value.input).join("\n");
    for (const marker of ["[", "]", "{", "}", "@", "^", "~", ":", "1.", "+---+", "|"]) {
        assert.ok(inputs.includes(marker), marker);
    }
});
