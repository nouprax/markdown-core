import assert from "node:assert/strict";
import test from "node:test";
import { parseElementInventory } from "../lib/element-inventory.mjs";

const symbol = (name) => `MARKDOWN_CORE_ELEMENT_${name}`;
const definition = (name) => `const markdown_core_element ${symbol(name)} = {\n    .dispatch = "[!",\n};\n`;
const table = (names) =>
    `static const markdown_core_element *const CORE_ELEMENTS[] = {\n${names
        .map((name) => `    &${symbol(name)}`)
        .join(",\n")}\n};\n`;
const sources = (defined, attached = defined) => [
    { file: "core-elements.c", source: table(attached) },
    ...defined.map((name) => ({ file: `${name.toLowerCase()}.c`, source: definition(name) }))
];

test("inventory follows descriptor identity and attach order at any feature count", () => {
    for (const count of [1, 2, 6, 7, 11]) {
        const names = Array.from({ length: count }, (_, i) => `FEATURE_${String(i)}`);
        const order = [...names].reverse();
        const inventory = parseElementInventory(sources(names, order));
        assert.equal(inventory.descriptors.length, count);
        assert.deepEqual(
            inventory.ordered.map((entry) => entry.symbol),
            order.map(symbol)
        );
        assert.ok(inventory.descriptors.every((entry) => entry.body.includes('.dispatch = "[!"')));
    }
});

test("multiple descriptors in one source are all audited", () => {
    const inventory = parseElementInventory([
        { file: "core-elements.c", source: table(["SECOND", "FIRST"]) },
        { file: "features.c", source: definition("FIRST") + definition("SECOND") }
    ]);
    assert.deepEqual(
        inventory.ordered.map((entry) => entry.symbol),
        [symbol("SECOND"), symbol("FIRST")]
    );
});

test("missing and malformed inventories fail closed", () => {
    assert.throws(() => parseElementInventory([]), /exactly one CORE_ELEMENTS/);
    assert.throws(() => parseElementInventory(sources([])), /is empty/);
    assert.throws(
        () => parseElementInventory([{ file: "core-elements.c", source: table(["FIRST"]) + table(["FIRST"]) }]),
        /exactly one CORE_ELEMENTS/
    );
    const malformed = sources(["FIRST"]);
    malformed[1].source = `const markdown_core_element ${symbol("FIRST")} = other;\n`;
    assert.throws(() => parseElementInventory(malformed), /could not read every/);
    const unreadable = sources(["FIRST"]);
    unreadable[0].source = unreadable[0].source.replace(`&${symbol("FIRST")}`, "other");
    assert.throws(() => parseElementInventory(unreadable), /unreadable CORE_ELEMENTS/);
});

test("identity mismatches fail even when descriptor counts agree", () => {
    assert.throws(() => parseElementInventory(sources(["FIRST"], ["SECOND"])), /has no definition/);
    assert.throws(() => parseElementInventory(sources(["FIRST", "SECOND"], ["FIRST"])), /has no CORE_ELEMENTS/);
    assert.throws(() => parseElementInventory(sources(["FIRST"], ["FIRST", "FIRST"])), /duplicate CORE_ELEMENTS/);
    assert.throws(() => parseElementInventory(sources(["FIRST", "FIRST"], ["FIRST"])), /duplicate descriptor/);
});

test("delimiter projections cannot silently overwrite another element", () => {
    const input = sources(["FIRST", "SECOND"]);
    const rule = (name, character) =>
        `.delimiter_rule = MARKDOWN_CORE_DELIM_RULE_${name},\n` +
        `.delimiter_character = '${character}',\n` +
        ".delimiter = {.minimum_width = 2, .maximum_width = 2},\n";
    const set = (index, fields) => {
        input[index].source = definition(index === 1 ? "FIRST" : "SECOND").replace(".dispatch", `${fields}.dispatch`);
    };
    set(1, rule("MARK", "="));
    set(2, rule("INSERTION", "+"));
    assert.doesNotThrow(() => parseElementInventory(input));
    set(2, rule("MARK", "+"));
    assert.throws(() => parseElementInventory(input), /duplicate delimiter rule/);
    set(2, rule("INSERTION", "="));
    assert.throws(() => parseElementInventory(input), /duplicate default delimiter character/);
    set(2, rule("EMPHASIS", "*"));
    assert.doesNotThrow(() => parseElementInventory(input));
    set(2, rule("NONE", "*"));
    assert.throws(() => parseElementInventory(input), /reserved by the engine/);
    set(2, rule("INSERTION", "+").replace("minimum_width = 2", "minimum_width = 3"));
    assert.throws(() => parseElementInventory(input), /invalid parsed delimiter widths/);
});

test("block scanners declare their indentation bound", () => {
    const input = sources(["FIRST"]);
    const set = (fields) => {
        input[1].source = definition("FIRST").replace(".dispatch", `${fields}.dispatch`);
    };
    set(".scan_block_start = scan,\n");
    assert.throws(() => parseElementInventory(input), /explicit indentation bound/);
    set(".scan_block_start = scan,\n.maximum_block_indent = 3,\n");
    assert.doesNotThrow(() => parseElementInventory(input));
    set(".scan_block_start = scan,\n.maximum_block_indent = INT_MAX,\n");
    assert.doesNotThrow(() => parseElementInventory(input));
});
