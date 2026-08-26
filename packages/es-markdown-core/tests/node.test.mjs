import assert from "node:assert/strict";
import { test } from "node:test";
import { TextEncoder } from "node:util";
import { Document, ParseError, TreeDumper, visit, Walker, WalkEvent } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { copyOut, discardOut } from "../dist/runtime/parser.js";
import { NodeDecoder } from "../dist/wire/node-decoder.js";
import { kindVisitor } from "./visitor.mjs";

// The whole-text parse: the one entry, sealed in the same breath. `parse`
// keeps only the semantic tree; the concrete and lifecycle tests below spell
// the full entry out themselves.
const parse = (source, options) => new Document(source, options).seal().semantic;

test("api: synchronous parse, typed visitor dispatch, and walker", () => {
    const document = parse("# Heading\n\nBody\n");
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
    assert.equal(parse(markdown).content[0].kind, "table");
    assert.equal(parse(markdown, { tables: false }).content[0].kind, "paragraph");
});

test("ast: typed fields are copied from direct WASM accessors", () => {
    const document = parse("3. item\n\n| a |\n| :-: |\n| b |\n");
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.deepEqual(document.content[1].alignments, ["center"]);
});

test("ast: every Markup exposes the canonical diagnostic dump", () => {
    const document = parse("# Heading\n");
    assert.equal(document.dump(), TreeDumper.dump(document));
    assert.match(document.content[0].dump(), /^Heading scope=/);
    assert.equal(Object.keys(document).includes("dump"), false);
});

test("unicode: UTF-8 survives native document release", () => {
    const document = parse("héllo 🚀 中文\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
    for (let index = 0; index < 300; index += 1) parse("# copy\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
});

test("errors: empty input is valid and arguments are checked", () => {
    assert.deepEqual(parse("").content, []);
    assert.throws(() => new Document(null), TypeError);
    assert.throws(() => new Document(42), TypeError);
    assert.throws(() => parse("x", { tables: "yes" }), TypeError);
    assert.throws(() => parse("x", { tables: null }), TypeError);
});

test("ownership: declarations are readonly without runtime freeze", () => {
    const document = parse("text\n");
    assert.equal(Object.isFrozen(document), false);
    assert.equal(Object.isFrozen(document.content), false);
});

test("robustness: large documents copy completely before native release", () => {
    const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    assert.equal(parse(unit.repeat(5_000)).content.length, 10_000);
});

test("robustness: deep block quote nesting remains traversable", () => {
    const depth = 128;
    let node = parse("> ".repeat(depth) + "leaf\n").content[0];
    for (let index = 0; index < depth; index += 1) {
        assert.equal(node.kind, "blockQuote");
        node = node.content[0];
    }
    assert.equal(node.kind, "paragraph");
});

test("robustness: repeated parse and release remains stable", () => {
    for (let index = 0; index < 2_000; index += 1) {
        assert.equal(parse("# Copy\n\n- [x] item 🚀\n").content.length, 2);
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
    const read = new Document(source).seal();
    const concrete = read.concrete;
    assert.deepEqual(concrete.source, new TextEncoder().encode(source));
    assert.equal(concrete.lines, 15);
    assert.equal(concrete.offset(1), 0);
    assert.equal(concrete.offset(3), 14);
    assert.throws(() => concrete.offset(0), RangeError);
    assert.throws(() => concrete.offset(16), RangeError);

    // Every line but the first begins after a line ending.
    for (let line = 2; line <= concrete.lines; line += 1) {
        const start = concrete.offset(line);
        assert.ok(start > 0);
        assert.equal(concrete.source[start - 1], "\n".charCodeAt(0));
    }

    // Nothing native is left: 300 more parses cannot move what was copied.
    for (let index = 0; index < 300; index += 1) parse("# copy\n");
    assert.deepEqual(concrete.source, new TextEncoder().encode(source));
    assert.equal(concrete.offset(3), 14);
});

test("robustness: the heap grows, and a document larger than the initial one parses", () => {
    // The default heap is 16 MiB and a parse needs several times its input, so
    // before -sALLOW_MEMORY_GROWTH=1 anything past about 1.6 MiB did not fail --
    // it stopped returning. A fixed reservation only moves that cliff.
    const paragraph = "lorem ipsum dolor sit amet consectetur adipiscing elit\n\n";
    const source = paragraph.repeat(Math.round((4 * 1024 * 1024) / paragraph.length));
    const before = native.memory.buffer.byteLength;

    const document = parse(source);

    // The heap GREW rather than merely having been large enough to start with,
    // which is the other way this could have been made to pass and is the one
    // that leaves the cliff in place.
    assert.ok(native.memory.buffer.byteLength > before, "parsing 4 MiB must have grown the heap");
    assert.equal(document.content.length, Math.round((4 * 1024 * 1024) / paragraph.length));
    assert.equal(document.content[0].kind, "paragraph");

    // Read a string AFTER the growth: every view this runtime takes must be
    // constructed after the last call that could have detached the buffer, and
    // a stale one throws here rather than anywhere a user would see it.
    const withLink = parse(`${source}[a](/u "t")\n`);
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
    const document = parse(
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

// THE STREAM (docs/STREAMING.md §4 D5, under 3.0's names). Everything below
// reads a `Read` value the document returned, and nothing native stands
// behind it: a feed's answer outlives every later feed, the seal, and the
// document itself.
test("api: a document's chunked feeds equal the whole-text parse once sealed", () => {
    // CRLF line endings and characters of two, three and four UTF-8 bytes, so
    // a 7-byte chunk boundary lands inside a line ending and inside every
    // multi-byte width there is -- the splits only a byte stream can spell.
    const source = "# Héllo 🚀 中文\r\n\r\n> quoted *em* and `code`\r\n\r\n- [x] tick\r\nsee [a] and $x$.\r\n";
    const bytes = new TextEncoder().encode(source);
    const document = new Document();
    try {
        for (let offset = 0; offset < bytes.length; offset += 7) {
            document.feed(bytes.subarray(offset, Math.min(offset + 7, bytes.length)));
        }
        const sealed = document.seal();
        const wholeText = new Document(source).seal();
        assert.equal(sealed.dump(), wholeText.dump());
        assert.deepEqual(sealed.concrete.source, wholeText.concrete.source);
        assert.equal(sealed.concrete.lines, wholeText.concrete.lines);
        for (let line = 1; line <= wholeText.concrete.lines; line += 1) {
            assert.equal(sealed.concrete.offset(line), wholeText.concrete.offset(line));
        }
    } finally {
        document.dispose();
    }
});

test("api: a document reads the same options however it was opened", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    assert.equal(new Document(markdown, { tables: false }).seal().semantic.content[0].kind, "paragraph");
    assert.equal(new Document(undefined, { tables: false }).feed(markdown).semantic.content[0].kind, "paragraph");
    const gated = new Document({ tables: false });
    try {
        // The canonical spelling: a string chunk feeds its UTF-8 bytes.
        assert.equal(gated.feed(markdown).semantic.content[0].kind, "paragraph");
        assert.equal(gated.seal().semantic.content[0].kind, "paragraph");
    } finally {
        gated.dispose();
    }
    const open = new Document();
    try {
        open.feed(markdown);
        assert.equal(open.seal().semantic.content[0].kind, "table");
    } finally {
        open.dispose();
    }
});

test("api: an empty feed is legal and an unfed document seals to the empty read", () => {
    const document = new Document();
    try {
        assert.deepEqual(document.feed("").semantic.content, []);
        assert.deepEqual(document.feed(new Uint8Array(0)).semantic.content, []);
        const sealed = document.seal();
        assert.deepEqual(sealed.semantic.content, []);
        assert.equal(sealed.dump(), new Document("").seal().dump());
    } finally {
        document.dispose();
    }
});

test("ownership: a mid-stream read is a value later feeds cannot disturb", () => {
    const document = new Document();
    // The trailing line's ending has not arrived, so "tail" is not yet in the
    // projection -- not in the tree and not in the concrete view.
    const early = document.feed("# Heading\n\ntail");
    assert.equal(early.semantic.content.length, 1);
    assert.equal(early.semantic.content[0].kind, "heading");
    assert.deepEqual(early.concrete.source, new TextEncoder().encode("# Heading\n\n"));
    const record = early.dump();

    // A later feed completes the line; the value already returned does not
    // move, and the new answer carries the completed line.
    const grown = document.feed(" grows\n");
    assert.equal(early.dump(), record);
    assert.equal(early.semantic.content.length, 1);
    assert.equal(grown.semantic.content[1].kind, "paragraph");
    assert.equal(grown.semantic.content[1].content[0].literal, "tail grows");

    // The sealed read equals the whole-text parse of the same bytes, and
    // every earlier answer survives the document's death.
    const sealed = document.seal();
    assert.equal(sealed.dump(), new Document("# Heading\n\ntail grows\n").seal().dump());
    assert.equal(early.dump(), record);
    assert.equal(early.semantic.content[0].kind, "heading");
});

test("errors: sealing releases the shell, and a sealed or disposed document refuses everything", () => {
    const document = new Document("done\n");
    document.seal();
    // Sealing IS disposing: nothing native remains, so a later call is a use
    // of a dead object rather than a parse error crossing the wire.
    assert.throws(() => document.feed("late\n"), /sealed or disposed/u);
    assert.throws(() => document.seal(), /sealed or disposed/u);

    // Disposal is idempotent on what is already gone, and `using` disposes
    // through the same door.
    document.dispose();
    document.dispose();
    document[Symbol.dispose]();
    assert.throws(() => document.feed("x"), /sealed or disposed/u);
    assert.throws(() => document.seal(), /sealed or disposed/u);

    // Arguments are checked up front, in both constructor forms.
    assert.throws(() => new Document(null), TypeError);
    assert.throws(() => new Document({ tables: "yes" }), TypeError);
    assert.throws(() => new Document(42, {}), TypeError);
    const typed = new Document();
    try {
        assert.throws(() => typed.feed(42), TypeError);
    } finally {
        typed.dispose();
    }
});

test("api: the constructor's initial chunk is bytes as much as text", () => {
    const bytes = new TextEncoder().encode("# Bytes\n");
    const read = new Document(bytes).seal();
    assert.equal(read.semantic.content[0].kind, "heading");
    assert.equal(read.dump(), new Document("# Bytes\n").seal().dump());
});

test("errors: ParseError carries its code, name, and message", () => {
    // No text a caller can write produces one -- a parse failure is an
    // allocation failure -- so the exported class is pinned directly: it is
    // what a consumer's catch narrows on.
    const error = new ParseError("allocationFailed", "out of memory");
    assert.ok(error instanceof Error);
    assert.equal(error.name, "ParseError");
    assert.equal(error.code, "allocationFailed");
    assert.equal(error.message, "out of memory");
});

test("errors: a read that never materialized is a ParseError, not a crash", () => {
    // `copyOut` throwing is unreachable through text -- a parse failure is an
    // allocation failure -- so it is reached the same way `discardOut`'s arm
    // is: a call that answers with no document and no error behind the slot.
    assert.throws(
        () => copyOut(() => 0),
        (error) => error instanceof ParseError && error.code === "internal"
    );
});

test("errors: a discarded feed still surfaces a native failure and frees its slot", () => {
    // The constructor's initial feed discards its read, and text never fails,
    // so the error arm is reached the way the heap tests reach the runtime:
    // past index.js, handing `discardOut` a call that answers with no
    // document and no error behind the slot.
    assert.throws(
        () => discardOut(() => 0),
        (error) => error instanceof ParseError && error.code === "internal"
    );
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
});
