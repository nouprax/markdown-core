import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

import { buildModel, outputs } from "../generate-node-kinds.mjs";

const read = (relative) => fs.readFileSync(new URL(`../../../${relative}`, import.meta.url), "utf8");
const contract = () => JSON.parse(read("docs/specs/canonical-ast.json"));
const native = () => JSON.parse(read("packages/markdown-core/node-types.json"));

test("each class is numbered densely from 1 in schema order", () => {
    const model = buildModel(contract(), native());
    for (const klass of [model.block, model.inline]) {
        assert.deepEqual(
            klass.types.map((type) => type.value),
            klass.types.map((_, index) => index + 1)
        );
    }
    assert.equal(model.count, Math.max(model.block.types.length, model.inline.types.length) + 1);
});

test("every committed output is what the schemas generate", () => {
    for (const [relative, text] of outputs(buildModel(contract(), native()), read)) {
        assert.equal(read(relative), text, `${relative} is stale; run scripts/tooling/generate-node-kinds.mjs`);
    }
});

test("the facade reports each public kind from exactly the native types that name it", () => {
    const model = buildModel(contract(), native());
    const ast = outputs(model, read).get("packages/markdown-core/elements/ast.c");
    for (const type of [...model.block.types, ...model.inline.types]) {
        const row = new RegExp(
            `\\[MARKDOWN_CORE_NODE_${type.name} & MARKDOWN_CORE_NODE_VALUE_MASK\\] = MARKDOWN_CORE_KIND_`
        );
        assert.equal(row.test(ast), type.kind !== null, type.name);
    }
});

test("content kinds are exactly the kinds no field names, the Document aside", () => {
    const model = buildModel(contract(), native());
    const content = model.kinds.filter((kind) => kind.content).map((kind) => kind.name);
    for (const owned of ["Document", "Metadata", "Citation", "ListItem", "TableRow"]) {
        assert.ok(!content.includes(owned), owned);
    }
    for (const kind of ["Paragraph", "Text", "Cite", "Table", "DefinitionList", "Comment", "Footnote", "Specimen"]) {
        assert.ok(content.includes(kind), kind);
    }
    const retyped = contract();
    retyped.kinds.find((kind) => kind.name === "Paragraph").fields.push({ name: "x", type: "[Text]" });
    assert.ok(!buildModel(retyped, native()).kinds.find((kind) => kind.name === "Text").content);
});

test("ordinals must run from 1 without a gap or a repeat", () => {
    for (const ordinal of [0, 2, 44]) {
        const broken = contract();
        broken.kinds[0].ordinal = ordinal;
        assert.throws(() => buildModel(broken, native()), /ordinals must run 1\.\.43 once each/);
    }
});

test("a native type must name a contract kind or null", () => {
    const broken = native();
    broken.block[0].kind = "Nonexistent";
    assert.throws(() => buildModel(contract(), broken), /kind Nonexistent is not a kind of/);
});

test("every contract kind must be reported by a native type", () => {
    const broken = native();
    broken.inline = broken.inline.filter((type) => type.kind !== "Subscript");
    assert.throws(() => buildModel(contract(), broken), /no native type reports the public kind Subscript/);
});

test("a native type states every column, even when it is null", () => {
    const broken = native();
    delete broken.inline[0].structure;
    assert.throws(() => buildModel(contract(), broken), /structure must be given, as null/);
});

test("a native type name is used once across both classes", () => {
    const broken = native();
    broken.inline.push({ ...broken.block[0] });
    assert.throws(() => buildModel(contract(), broken), /DOCUMENT: named twice/);
});

test("a hand-written file carries exactly one generated region", () => {
    const model = buildModel(contract(), native());
    const without = (relative) => read(relative).replace(/\/\* BEGIN GENERATED[^\n]*\n/, "");
    assert.throws(() => outputs(model, without), /needs exactly one generated region/);
    const twice = (relative) => read(relative) + read(relative);
    assert.throws(() => outputs(model, twice), /needs exactly one generated region/);
});
