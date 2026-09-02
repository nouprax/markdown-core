import assert from "node:assert/strict";
import { test } from "node:test";
import { TextEncoder } from "node:util";
import { Document, TreeDumper, visit, Walker, WalkEvent } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { NodeDecoder } from "../dist/wire/node-decoder.js";
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

test("ast: every Markup exposes the canonical debug dump", () => {
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
test("concrete: the normalized source and its line index survive the native release", () => {
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

    // Every line but the first begins after a line ending.
    for (let line = 2; line <= concrete.lineCount; line += 1) {
        const start = concrete.lineStart(line);
        assert.ok(start > 0);
        assert.equal(concrete.source[start - 1], "\n".charCodeAt(0));
    }

    // Nothing native is left: 300 more parses cannot move what was copied.
    for (let index = 0; index < 300; index += 1) Document.parse("# copy\n");
    assert.deepEqual(concrete.source, new TextEncoder().encode(source));
    assert.equal(concrete.lineStart(3), 14);
});

test("robustness: the heap grows, and a document larger than the initial one parses", () => {
    // The default heap is 16 MiB and a parse needs several times its input, so
    // before -sALLOW_MEMORY_GROWTH=1 anything past about 1.6 MiB did not fail --
    // it stopped returning. A fixed reservation only moves that cliff.
    const paragraph = "lorem ipsum dolor sit amet consectetur adipiscing elit\n\n";
    const source = paragraph.repeat(Math.round((4 * 1024 * 1024) / paragraph.length));
    const before = native.memory.buffer.byteLength;

    const document = Document.parse(source);

    // The heap GREW rather than merely having been large enough to start with,
    // which is the other way this could have been made to pass and is the one
    // that leaves the cliff in place.
    assert.ok(native.memory.buffer.byteLength > before, "parsing 4 MiB must have grown the heap");
    assert.equal(document.content.length, Math.round((4 * 1024 * 1024) / paragraph.length));
    assert.equal(document.content[0].kind, "paragraph");

    // Read a string AFTER the growth: every view this runtime takes must be
    // constructed after the last call that could have detached the buffer, and
    // a stale one throws here rather than anywhere a user would see it.
    const withLink = Document.parse(`${source}[a](/u "t")\n`);
    const link = withLink.content.at(-1).content[0];
    assert.equal(link.kind, "link");
    assert.equal(link.destination, "/u");
    assert.equal(link.title, "t");
});

test("ast: the decoder's reference, formula, list and empty-string arms are exercised", () => {
    // Four decoder arms that no other suite reaches, and each is an ordinary
    // language feature rather than a defensive branch: a reference's form, a
    // formula's placement, an ordered list's flavour, and requirement 14's
    // "written and empty" answer, which is the one a `null` would be confused
    // with.
    const document = Document.parse(
        ['[foo]: /url "t"', "", "See [foo] and $$x$$ and [a]().", "", "3. one", "4. two", ""].join("\n")
    );

    const [definition, paragraph, list] = document.content;
    assert.equal(definition.kind, "referenceDefinition");
    assert.equal(definition.label, "foo");
    assert.equal(definition.destination, "/url");

    const reference = paragraph.content.find((node) => node.kind === "linkReference");
    assert.equal(reference.form, "shortcut");
    assert.equal(reference.identifier, "foo");

    const formula = paragraph.content.find((node) => node.kind === "formula");
    assert.equal(formula.mode, "standalone");
    assert.equal(formula.literal, "x");

    // `[a]()` WROTE a destination and wrote nothing in it. Empty is not absent.
    const link = paragraph.content.find((node) => node.kind === "link");
    assert.equal(link.destination, "");
    assert.notEqual(link.destination, null);

    assert.equal(list.kind, "list");
    assert.equal(list.flavor, "ordered");
    assert.equal(list.start, 3);
    assert.equal(list.items.length, 2);
});

test("errors: every wire guard fires when the native side answers out of range", () => {
    // These guards exist because the two sides of the wire are versioned
    // separately -- the Kotlin bridge's bump to MKC5 is the same hazard -- and
    // a decoder that silently mapped an unknown value would turn a protocol
    // mismatch into a wrong document. Nothing proved any of them fires, so a
    // renumbering could have removed the check and stayed green.
    const decoder = new NodeDecoder(native);
    try {
        assert.throws(() => decoder.referenceForm(9), /invalid reference form 9/u);
        assert.throws(() => decoder.placement(9), /invalid placement mode 9/u);
        assert.throws(() => decoder.listFlavor(9), /invalid list flavor 9/u);
        assert.throws(() => decoder.tableAlignment(9), /invalid table alignment 9/u);
        assert.throws(() => decoder.boolean(9, "checked"), /invalid checked 9/u);
        assert.throws(() => decoder.count(-1, "column count"), /invalid column count -1/u);

        // The valid answers still map, so the guards reject rather than
        // everything throwing for some unrelated reason.
        assert.equal(decoder.referenceForm(3), "shortcut");
        assert.equal(decoder.placement(2), "standalone");
        assert.equal(decoder.listFlavor(2), "ordered");
        assert.equal(decoder.tableAlignment(0), "none");
        assert.equal(decoder.nullableBoolean(-1, "checked"), null);
    } finally {
        decoder.dispose();
    }

    // A disposed decoder holds a freed scratch pointer, and reading through it
    // would be a use-after-free in WASM memory rather than an error.
    assert.throws(() => decoder.requireLive(), /decoder has been disposed/u);

    // Native parse failures keep their terminal category across the WASM
    // boundary. In particular, allocation failure must not be collapsed into
    // an internal error that a consumer could mistake for a recoverable path.
    let rawErrorCode = 1;
    const errorDecoder = new NodeDecoder({
        memory: new WebAssembly.Memory({ initial: 1 }),
        malloc: () => 8,
        free: () => {},
        es_error_code: () => rawErrorCode,
        es_string: () => 0
    });
    try {
        assert.equal(errorDecoder.parseError(1).code, "invalidArgument");
        rawErrorCode = 2;
        assert.equal(errorDecoder.parseError(1).code, "allocationFailed");
        rawErrorCode = 99;
        assert.equal(errorDecoder.parseError(1).code, "internal");
        assert.equal(errorDecoder.parseError(0).code, "internal");
    } finally {
        errorDecoder.dispose();
    }

    // A directive label is a typed field with its own node kind. Accepting a
    // generic child here would erase the structural distinction this wire
    // contract exists to preserve.
    const malformedDirectiveDecoder = new NodeDecoder({
        memory: new WebAssembly.Memory({ initial: 1 }),
        malloc: () => 8,
        free: () => {},
        es_node_directive_label: () => 2,
        es_node_kind: () => 3,
        es_node_first_child: () => 0,
        es_scope_coordinate: () => 1
    });
    try {
        assert.throws(
            () => malformedDirectiveDecoder.directiveFields(1),
            /directive label field contains a non-label node/u
        );
    } finally {
        malformedDirectiveDecoder.dispose();
    }
});
