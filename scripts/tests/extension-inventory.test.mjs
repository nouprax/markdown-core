import assert from "node:assert/strict";
import test from "node:test";
import { parseExtensionInventory } from "../lib/extension-inventory.mjs";

const symbol = (name) => `MARKDOWN_CORE_EXTENSION_${name}`;
const definition = (name) => `const markdown_core_extension ${symbol(name)} = {\n    .dispatch = "[!",\n};\n`;
const table = (names) =>
    `static const markdown_core_extension *const CORE_EXTENSIONS[] = {\n${names
        .map((name) => `    &${symbol(name)}`)
        .join(",\n")}\n};\n`;
const sources = (defined, attached = defined) => [
    { file: "core-extensions.c", source: table(attached) },
    ...defined.map((name) => ({ file: `${name.toLowerCase()}.c`, source: definition(name) }))
];

test("inventory follows descriptor identity and attach order at any feature count", () => {
    for (const count of [1, 2, 6, 7, 11]) {
        const names = Array.from({ length: count }, (_, i) => `FEATURE_${String(i)}`);
        const order = [...names].reverse();
        const inventory = parseExtensionInventory(sources(names, order));
        assert.equal(inventory.descriptors.length, count);
        assert.deepEqual(
            inventory.ordered.map((entry) => entry.symbol),
            order.map(symbol)
        );
        assert.ok(inventory.descriptors.every((entry) => entry.body.includes('.dispatch = "[!"')));
    }
});

test("multiple descriptors in one source are all audited", () => {
    const inventory = parseExtensionInventory([
        { file: "core-extensions.c", source: table(["SECOND", "FIRST"]) },
        { file: "features.c", source: definition("FIRST") + definition("SECOND") }
    ]);
    assert.deepEqual(
        inventory.ordered.map((entry) => entry.symbol),
        [symbol("SECOND"), symbol("FIRST")]
    );
});

test("missing and malformed inventories fail closed", () => {
    assert.throws(() => parseExtensionInventory([]), /exactly one CORE_EXTENSIONS/);
    assert.throws(() => parseExtensionInventory(sources([])), /is empty/);
    assert.throws(
        () => parseExtensionInventory([{ file: "core-extensions.c", source: table(["FIRST"]) + table(["FIRST"]) }]),
        /exactly one CORE_EXTENSIONS/
    );
    const malformed = sources(["FIRST"]);
    malformed[1].source = `const markdown_core_extension ${symbol("FIRST")} = other;\n`;
    assert.throws(() => parseExtensionInventory(malformed), /could not read every/);
    const unreadable = sources(["FIRST"]);
    unreadable[0].source = unreadable[0].source.replace(`&${symbol("FIRST")}`, "other");
    assert.throws(() => parseExtensionInventory(unreadable), /unreadable CORE_EXTENSIONS/);
});

test("identity mismatches fail even when descriptor counts agree", () => {
    assert.throws(() => parseExtensionInventory(sources(["FIRST"], ["SECOND"])), /has no definition/);
    assert.throws(() => parseExtensionInventory(sources(["FIRST", "SECOND"], ["FIRST"])), /has no CORE_EXTENSIONS/);
    assert.throws(() => parseExtensionInventory(sources(["FIRST"], ["FIRST", "FIRST"])), /duplicate CORE_EXTENSIONS/);
    assert.throws(() => parseExtensionInventory(sources(["FIRST", "FIRST"], ["FIRST"])), /duplicate descriptor/);
});
