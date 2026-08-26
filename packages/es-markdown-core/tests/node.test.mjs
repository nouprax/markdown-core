import assert from "node:assert/strict";
import { test } from "node:test";
import { TextEncoder } from "node:util";
import { Document, ParseError, Session, TreeDumper, visit, Walker, WalkEvent } from "../dist/index.js";
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

// THE STREAM (docs/STREAMING.md §4 D5). Everything below reads a document
// value the session returned, and nothing native stands behind it: a feed's
// answer outlives every later feed, the finish, and the session itself.
test("api: a session's chunked feeds sealed by finish equal the one-shot parse", () => {
    // CRLF line endings and characters of two, three and four UTF-8 bytes, so
    // a 7-byte chunk boundary lands inside a line ending and inside every
    // multi-byte width there is -- the splits only a byte stream can spell.
    const source = "# Héllo 🚀 中文\r\n\r\n> quoted *em* and `code`\r\n\r\n- [x] tick\r\nsee [a] and $x$.\r\n";
    const bytes = new TextEncoder().encode(source);
    const session = new Session();
    try {
        for (let offset = 0; offset < bytes.length; offset += 7) {
            session.feed(bytes.subarray(offset, Math.min(offset + 7, bytes.length)));
        }
        const sealed = session.finish();
        const oneShot = Document.parse(source);
        assert.equal(sealed.dump(), oneShot.dump());
        assert.deepEqual(sealed.concrete.source, oneShot.concrete.source);
        assert.equal(sealed.concrete.lineCount, oneShot.concrete.lineCount);
        for (let line = 1; line <= oneShot.concrete.lineCount; line += 1) {
            assert.equal(sealed.concrete.lineStart(line), oneShot.concrete.lineStart(line));
        }
    } finally {
        session.dispose();
    }
});

test("api: a session reads the same options the one-shot parse does", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    const gated = new Session({ tables: false });
    try {
        // The canonical spelling: a string chunk feeds its UTF-8 bytes.
        assert.equal(gated.feed(markdown).content[0].kind, "paragraph");
        assert.equal(gated.finish().content[0].kind, "paragraph");
    } finally {
        gated.dispose();
    }
    const open = new Session();
    try {
        open.feed(markdown);
        assert.equal(open.finish().content[0].kind, "table");
    } finally {
        open.dispose();
    }
});

test("api: an empty feed is legal and an unfed session seals to the empty document", () => {
    const session = new Session();
    try {
        assert.deepEqual(session.feed("").content, []);
        assert.deepEqual(session.feed(new Uint8Array(0)).content, []);
        const sealed = session.finish();
        assert.deepEqual(sealed.content, []);
        assert.equal(sealed.dump(), Document.parse("").dump());
    } finally {
        session.dispose();
    }
});

test("ownership: a mid-stream document is a value later feeds cannot disturb", () => {
    const session = new Session();
    // The trailing line's ending has not arrived, so "tail" is not yet in the
    // projection -- not in the tree and not in the concrete view.
    const early = session.feed("# Heading\n\ntail");
    assert.equal(early.content.length, 1);
    assert.equal(early.content[0].kind, "heading");
    assert.deepEqual(early.concrete.source, new TextEncoder().encode("# Heading\n\n"));
    const record = early.dump();

    // A later feed completes the line; the value already returned does not
    // move, and the new answer carries the completed line.
    const grown = session.feed(" grows\n");
    assert.equal(early.dump(), record);
    assert.equal(early.content.length, 1);
    assert.equal(grown.content[1].kind, "paragraph");
    assert.equal(grown.content[1].content[0].literal, "tail grows");

    // The sealed document equals the one-shot parse of the same bytes, and
    // every earlier answer survives the session's death.
    const sealed = session.finish();
    session.dispose();
    assert.equal(sealed.dump(), Document.parse("# Heading\n\ntail grows\n").dump());
    assert.equal(early.dump(), record);
    assert.equal(early.content[0].kind, "heading");
});

test("errors: a sealed session refuses feed and finish, and a disposed one refuses everything", () => {
    const session = new Session();
    session.feed("done\n");
    session.finish();
    // The stream is sealed: the refusal is the parser's, crosses the wire as
    // an error, and names the code the C surface rules for it.
    const sealedRefusal = (error) => error instanceof ParseError && error.code === "invalidArgument";
    assert.throws(() => session.feed("late\n"), sealedRefusal);
    assert.throws(() => session.finish(), sealedRefusal);

    // Disposal is idempotent, and a disposed session refuses up front rather
    // than calling into freed native memory.
    session.dispose();
    session.dispose();
    assert.throws(() => session.feed("x"), /session has been disposed/u);
    assert.throws(() => session.finish(), /session has been disposed/u);

    // Arguments are checked the way `parse` checks its own.
    assert.throws(() => new Session(null), TypeError);
    assert.throws(() => new Session({ tables: "yes" }), TypeError);
    const typed = new Session();
    try {
        assert.throws(() => typed.feed(42), TypeError);
    } finally {
        typed.dispose();
    }
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
