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

test("ordinals must run from 1 without a gap or a repeat", () => {
    const count = contract().kinds.length;
    for (const ordinal of [0, 2, count + 1]) {
        const broken = contract();
        broken.kinds[0].ordinal = ordinal;
        assert.throws(() => buildModel(broken, native()), new RegExp(`ordinals must run 1\\.\\.${count} once each`));
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

test("a hand-written file carries each of its generated regions exactly once", () => {
    const model = buildModel(contract(), native());
    const without = (relative) => read(relative).replace(/\/\* BEGIN GENERATED node kinds[^\n]*\n/, "");
    assert.throws(() => outputs(model, without), /needs exactly one generated region of node kinds/);
    const twice = (relative) => read(relative) + read(relative);
    assert.throws(() => outputs(model, twice), /needs exactly one generated region of node kinds/);
    const header = "packages/markdown-core/include/markdown_core.h";
    const noSlots = (relative) =>
        relative === header ? read(relative).replace(/\/\* BEGIN GENERATED slot kinds[^\n]*\n/, "") : read(relative);
    assert.throws(() => outputs(model, noSlots), /markdown_core\.h: needs exactly one generated region of slot kinds/);
});

test("slot kinds are numbered densely from 1 and each names one lowercase word without g, j or y", () => {
    const model = buildModel(contract(), native());
    assert.deepEqual(
        model.slots.map((slot) => slot.ordinal),
        model.slots.map((_, index) => index + 1)
    );
    const gap = contract();
    gap.slots[0].ordinal = gap.slots.length + 1;
    assert.throws(() => buildModel(gap, native()), /slot ordinals must run 1\.\./);
    for (const name of ["key", "twoWords", "fragment"]) {
        const broken = contract();
        broken.slots[0].name = name;
        assert.throws(() => buildModel(broken, native()), /is not one lowercase word without g, j or y/);
    }
});

test("a field names declared slot kinds, and every slot kind is some field's", () => {
    const unknown = contract();
    unknown.kinds.find((kind) => kind.name === "Topic").fields[0].slots = ["tag"];
    assert.throws(() => buildModel(unknown, native()), /Topic\.name names the slot tag, which is not a slot kind/);
    const unused = contract();
    unused.kinds.find((kind) => kind.name === "Callout").fields[0].slots = [];
    assert.throws(() => buildModel(unused, native()), /no field holds the slot variant/);
});

test("a slot is owned by its field's kind, by the kinds holding its value, or by any kind", () => {
    const slots = new Map(buildModel(contract(), native()).slots.map((slot) => [slot.name, slot]));
    assert.deepEqual(slots.get("variant").owners, ["Callout"]);
    assert.deepEqual(slots.get("footnote").owners.sort(), ["Citation", "Footnote"]);
    assert.deepEqual(slots.get("label").owners.sort(), ["Embedded", "Link", "Reference"]);
    assert.equal(slots.get("class").any, true);
    assert.deepEqual(slots.get("class").owners, []);
    const broken = contract();
    broken.values.Destination.holders.cross.push("Topic");
    assert.throws(
        () => buildModel(broken, native()),
        /Destination\.holders\.cross names Topic, which holds no Destination/
    );
});
