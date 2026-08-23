import assert from "node:assert/strict";
import { test } from "node:test";
import { TextEncoder } from "node:util";
import { Document, RegionRole, TreeDumper, visit, Walker, WalkEvent } from "../dist/index.js";
import { kindVisitor } from "./visitor.mjs";

test("api: synchronous parse, typed visitor dispatch, and walker", () => {
    const document = Document.parse("# Heading\n\nBody\n");
    assert.equal(
        visit(document.content[0], {
            ...kindVisitor,
            visitHeading: (node) => `heading:${node.level}`
        }),
        "heading:1"
    );
    const events = [];
    new Walker().walk(document, (event, node) => events.push(`${event}-${node.kind}`));
    assert.equal(events[0], `${WalkEvent.entering}-document`);
    assert.equal(events.at(-1), `${WalkEvent.exiting}-document`);
});

test("api: options gate extensions", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    assert.equal(Document.parse(markdown).content[0].kind, "table");
    assert.equal(Document.parse(markdown, { tables: false }).content[0].kind, "paragraph");
});

test("ast: typed fields are copied from direct WASM accessors", () => {
    const document = Document.parse("3. item\n\n| a |\n| :-: |\n| b |\n");
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.deepEqual(document.content[1].alignments, ["center"]);
});

test("ast: every Markup exposes the canonical diagnostic dump", () => {
    const document = Document.parse("# Heading\n");
    assert.equal(document.dump(), TreeDumper.dump(document));
    assert.match(document.content[0].dump(), /^Heading scope=/);
    assert.equal(Object.keys(document).includes("dump"), false);
});

test("unicode: UTF-8 survives native document release", () => {
    const document = Document.parse("héllo 🚀 中文\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
    for (let index = 0; index < 300; index += 1) Document.parse("# copy\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
});

test("errors: empty input is valid and arguments are checked", () => {
    assert.deepEqual(Document.parse("").content, []);
    assert.throws(() => Document.parse(null), TypeError);
    assert.throws(() => Document.parse("x", { tables: "yes" }), TypeError);
    assert.throws(() => Document.parse("x", { tables: null }), TypeError);
});

test("ownership: declarations are readonly without runtime freeze", () => {
    const document = Document.parse("text\n");
    assert.equal(Object.isFrozen(document), false);
    assert.equal(Object.isFrozen(document.content), false);
});

test("robustness: large documents copy completely before native release", () => {
    const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    assert.equal(Document.parse(unit.repeat(5_000)).content.length, 10_000);
});

test("robustness: deep block quote nesting remains traversable", () => {
    const depth = 128;
    let node = Document.parse("> ".repeat(depth) + "leaf\n").content[0];
    for (let index = 0; index < depth; index += 1) {
        assert.equal(node.kind, "blockQuote");
        node = node.content[0];
    }
    assert.equal(node.kind, "paragraph");
});

test("robustness: repeated parse and release remains stable", () => {
    for (let index = 0; index < 2_000; index += 1) {
        assert.equal(Document.parse("# Copy\n\n- [x] item 🚀\n").content.length, 2);
    }
});

// The requirement's own sentence: the concrete view survives being copied into
// value types and the handle being freed. `parse` frees it before it returns,
// so everything below reads a view with no WASM memory left behind it.
test("concrete: the view is total and its owners resolve after native release", () => {
    const source = [
        "# Heading ##",
        "",
        "> quoted *em* and `code`",
        "",
        "| a | b |",
        "| --- | --- |",
        "| c | d |",
        "",
        ":::container[Title]{kind=demo}",
        "Body",
        ":::",
        "",
        '[a]: /u "t"',
        "",
        "see [a].",
        ""
    ].join("\n");
    const document = Document.parse(source);
    const concrete = document.concrete;
    assert.deepEqual(concrete.source, new TextEncoder().encode(source));
    assert.equal(concrete.lineCount, 15);
    assert.equal(concrete.lineStart(1), 0);
    assert.equal(concrete.lineStart(3), 14);
    assert.throws(() => concrete.lineStart(0), RangeError);
    assert.throws(() => concrete.lineStart(16), RangeError);
    assert.throws(() => concrete.region(concrete.regionCount), RangeError);

    let covered = 0;
    let markers = 0;
    for (let index = 0; index < concrete.regionCount; index += 1) {
        const region = concrete.region(index);
        assert.equal(region.start, covered);
        assert.ok(region.length > 0);
        covered += region.length;
        assert.ok(document.ownerOf(region) !== undefined);
        if (region.role === RegionRole.marker) markers += 1;
    }
    // The heading's closing `##`, the table's pipes and the definition's
    // punctuation are in no literal anywhere in the semantic tree, and the line
    // above says every byte of them is in a region here.
    assert.equal(covered, concrete.source.length);
    assert.ok(markers > 0);
    assert.equal(document.ownerOf({ start: 0, length: 1, role: RegionRole.content, owner: [99] }), undefined);

    // THE DESCENT IS THE C CHILD ORDER, not the value tree's named fields. A
    // table holds its header BEFORE its rows, so byte 42 -- the `a` of the
    // header row -- has to land on line 5 and not on line 7; a directive holds
    // its LABEL before its content, so byte 106 -- the `B` of `Body` -- has to
    // land on line 10 and not inside the label on line 9.
    assert.deepEqual(ownerAt(document, 42).scope.start, { line: 5, column: 3 });
    assert.deepEqual(ownerAt(document, 106).scope.start, { line: 10, column: 1 });

    // Nothing native is left: 300 more parses cannot move what was copied.
    for (let index = 0; index < 300; index += 1) Document.parse("# copy\n");
    assert.deepEqual(concrete.source, new TextEncoder().encode(source));
    assert.equal(concrete.region(0).start, 0);
});

/** The owner of the region the byte at `offset` belongs to. */
function ownerAt(document, offset) {
    for (let index = 0; index < document.concrete.regionCount; index += 1) {
        const region = document.concrete.region(index);
        if (offset >= region.start && offset < region.start + region.length) {
            return document.ownerOf(region);
        }
    }
    throw new Error(`no region holds byte ${offset}`);
}
