import assert from "node:assert/strict";
import { test } from "node:test";
import { TextEncoder } from "node:util";
import { Document, ParseError, TreeDumper, visit, Walker, WalkEvent } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { copyOut, discardOut } from "../dist/runtime/parser.js";
import { decodeRead } from "../dist/wire/wire-decoder.js";
import { kindVisitor } from "./visitor.mjs";

// The whole-text parse: the one entry, sealed in the same breath. `parse`
// keeps only the semantic tree; the lifecycle tests below spell
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

test("ast: typed fields are decoded from the wire payload", () => {
    const document = parse("3. item\n\n| a |\n| :-: |\n| b |\n");
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.deepEqual(document.content[1].alignments, ["center"]);
});

test("ast: every Markup exposes the canonical diagnostic dump", () => {
    const document = parse("# Heading\n");
    assert.equal(document.dump(), TreeDumper.dump(document));
    assert.match(document.content[0].dump(), /^Heading id=\d+:0 scope=/u);
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
    // THE EDGE: the reference names the definition it resolved to, and the
    // name is the definition node's own identity.
    assert.deepEqual(reference.definition, definition.id);
    assert.equal(definition.norm, "foo");

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
    // projection.
    const early = document.feed("# Heading\n\ntail");
    assert.equal(early.semantic.content.length, 1);
    assert.equal(early.semantic.content[0].kind, "heading");
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

test("api: a block keeps its identity across feeds and a reference names the first definition", () => {
    const document = new Document();
    try {
        // The heading is the element a consumer renders; later feeds and the
        // seal must keep calling it by the same name (D4) -- the render key.
        const first = document.feed("# Title\n\nsee [a] and [^n].\n\n");
        const heading = first.semantic.content[0];
        assert.equal(heading.kind, "heading");
        const second = document.feed("[a]: /first\n\n[a]: /second\n\n[^n]: note\n");
        assert.deepEqual(second.semantic.content[0].id, heading.id);
        const sealed = document.seal();
        assert.deepEqual(sealed.semantic.content[0].id, heading.id);

        // Duplicate definitions: both stay in the tree, and the reference
        // names the FIRST by identity -- its own match key is the winning
        // definition's norm.
        const definitions = sealed.semantic.content.filter((node) => node.kind === "referenceDefinition");
        assert.equal(definitions.length, 2);
        const paragraph = sealed.semantic.content[1];
        const reference = paragraph.content.find((node) => node.kind === "linkReference");
        assert.deepEqual(reference.definition, definitions[0].id);
        assert.equal(definitions[0].norm, "a");
        const footnote = sealed.semantic.content.find((node) => node.kind === "footnoteDefinition");
        const call = paragraph.content.find((node) => node.kind === "footnoteReference");
        assert.deepEqual(call.definition, footnote.id);
        assert.equal(footnote.norm, "^n");

        // An inline's identity is (owning block, ordinal): unique within its
        // paragraph, owned by it.
        for (const node of paragraph.content) assert.equal(node.id.block, paragraph.id.block);
        const ids = new Set(paragraph.content.map((node) => `${node.id.block}:${node.id.ordinal}`));
        assert.equal(ids.size, paragraph.content.length);
    } finally {
        document.dispose();
    }
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
    // `copyOut` throwing here is unreachable through text -- the one payload
    // the bridge cannot build is the one it could not allocate -- so it is
    // reached by handing it a call that answers with no payload at all.
    assert.throws(
        () => copyOut(() => 0),
        (error) => error instanceof ParseError && error.code === "allocationFailed"
    );
});

test("errors: a discarded feed still surfaces a native failure and frees its slot", () => {
    // The constructor's initial feed discards its read, and text never fails,
    // so the error arm is reached the way the heap tests reach the runtime:
    // past index.js, handing `discardOut` a call that answers with no payload.
    assert.throws(
        () => discardOut(() => 0),
        (error) => error instanceof ParseError && error.code === "allocationFailed"
    );
});

test("errors: every refusal the wire reader can make is reached by a payload", () => {
    // These guards exist because the two sides of the wire are versioned
    // separately -- the MKC8 bump is the same hazard -- and a decoder that
    // silently mapped an unknown value would turn a protocol mismatch into a
    // wrong document. The payloads the bridge actually writes are well
    // formed, so the malformed ones are written by hand: `MKC8` is the magic,
    // the byte after it is the status -- 1 means an error follows -- and a
    // healthy payload then leads with its frame byte, 0 for a whole tree.
    const payload = (...parts) => {
        const out = [];
        for (const part of parts) {
            if (typeof part === "string") out.push(...new TextEncoder().encode(part));
            else if (typeof part === "number") out.push(part & 0xff);
            else for (let shift = 0; shift < 4; shift += 1) out.push((part.int >> (shift * 8)) & 0xff);
        }
        return Uint8Array.from(out);
    };
    const int = (value) => ({ int: value });

    // A native error crosses as a code and a message, which is the only path
    // that builds a ParseError out of a payload.
    assert.throws(
        () => decodeRead(payload("MKC8", 1, int(1), int(3), "bad")),
        (error) => error instanceof ParseError && error.code === "invalidArgument" && error.message === "bad"
    );
    assert.throws(
        () => decodeRead(payload("MKC8", 1, int(99), int(1), "x")),
        (error) => error instanceof ParseError && error.code === "internal"
    );

    // A status that is neither, a magic from the wrong wire version, a frame
    // the reader does not know, a delta with nothing to be a delta against, a
    // root that is not a document, and a payload that stops mid-value.
    assert.throws(() => decodeRead(payload("MKC8", 2)), /unsupported native bridge status/u);
    assert.throws(() => decodeRead(payload("MKC7", 0, 0)), /invalid native bridge payload/u);
    assert.throws(() => decodeRead(payload("MKC8", 0, 7)), /unknown wire frame 7/u);
    assert.throws(() => decodeRead(payload("MKC8", 0, 1)), /delta frame with no previous read/u);
    assert.throws(() => decodeRead(payload("MKC8", 0, 0, 3)), /invalid document tree/u);
    assert.throws(() => decodeRead(payload("MKC8", 0, 0, 1, int(1), int(1))), /truncated native bridge payload/u);
    assert.throws(() => decodeRead(payload("MKC8", 1, int(1), int(-2))), /invalid native bridge string/u);
});

test("errors: every out-of-range value inside a node payload is refused", () => {
    // One malformed NODE per guard, each wrapped in a well-formed envelope and
    // document root, so the refusal is the node's own rather than the
    // envelope's. The builder mirrors the wire: kind byte, identity, scope,
    // then the kind's fields.
    const int = (value) => ({ int: value });
    const long = (value) => ({ long: value });
    const bytes = (...parts) => {
        const out = [];
        for (const part of parts) {
            if (typeof part === "string") out.push(...new TextEncoder().encode(part));
            else if (typeof part === "number") out.push(part & 0xff);
            else if ("long" in part)
                for (let shift = 0; shift < 8; shift += 1)
                    out.push(Number((BigInt(part.long) >> BigInt(shift * 8)) & 0xffn));
            else for (let shift = 0; shift < 4; shift += 1) out.push((part.int >> (shift * 8)) & 0xff);
        }
        return out;
    };
    const identity = (block, ordinal) => bytes(int(block), int(ordinal));
    const scope = () => bytes(int(1), int(1), int(1), int(1));
    // A whole-tree frame whose document root carries exactly one child, which
    // is the malformed node.
    const wrap = (...node) =>
        Uint8Array.from([
            ...bytes("MKC8", 0, 0, 1),
            ...identity(1, 0),
            ...scope(),
            ...bytes(int(1)),
            ...bytes(...node)
        ]);
    const child = (kind, ...fields) => wrap(kind, ...identity(2, 0), ...scope(), ...fields);

    assert.throws(() => decodeRead(child(99)), /unknown node kind 99/u);
    assert.throws(() => decodeRead(child(1)), /a document node cannot be a child/u);
    assert.throws(() => decodeRead(child(4, int(9))), /invalid heading level 9/u);
    assert.throws(() => decodeRead(child(6, int(9))), /invalid list flavor 9/u);
    assert.throws(() => decodeRead(child(6, int(1), long(3), 1, 1)), /start value for a bullet list/u);
    assert.throws(() => decodeRead(child(7, 7)), /invalid list item checked state 7/u);
    assert.throws(() => decodeRead(child(8, int(-1), int(-1), int(0), 9)), /invalid code fenced state 9/u);
    assert.throws(() => decodeRead(child(11, int(1), 9)), /invalid table alignment/u);
    assert.throws(() => decodeRead(child(19, int(9))), /invalid placement mode 9/u);
    assert.throws(() => decodeRead(child(31, int(0), int(9))), /invalid reference form 9/u);
    assert.throws(() => decodeRead(wrap(3, ...identity(2, 0), ...scope(), int(-1))), /invalid child count -1/u);

    const paragraph = () => bytes(3, ...identity(3, 0), ...scope(), int(0));
    assert.throws(
        () => decodeRead(child(6, int(1), long(0), 0, 0, int(1), ...paragraph())),
        /list contains a non-item node/u
    );
    assert.throws(() => decodeRead(child(11, int(0), int(1), ...paragraph())), /table contains a non-row node/u);
    const headerlessRow = () => bytes(27, ...identity(3, 0), ...scope(), 0, int(0));
    const headerRow = () => bytes(27, ...identity(4, 0), ...scope(), 1, int(0));
    assert.throws(() => decodeRead(child(11, int(0), int(1), ...headerlessRow())), /contains 0 header rows/u);
    // The header row is the table's first child on the wire -- the engine
    // opens a table with it, and a delta addresses the rows by position.
    assert.throws(
        () => decodeRead(child(11, int(0), int(2), ...headerlessRow(), ...headerRow())),
        /does not open with its header row/u
    );
    assert.throws(() => decodeRead(child(27, 0, int(1), ...paragraph())), /table row contains a non-cell node/u);
    assert.throws(
        () => decodeRead(child(25, int(0), 0, int(1))),
        /an absent directive attribute container cannot hold attributes/u
    );
    assert.throws(
        () => decodeRead(child(25, int(0), 0, int(0), int(1), ...paragraph())),
        /inline directive contains block content/u
    );

    // A complete, healthy payload with one byte too many: the reader must
    // refuse the surplus rather than decode a prefix and call it the document.
    const complete = Uint8Array.from([
        ...bytes("MKC8", 0, 0, 1),
        ...identity(1, 0),
        ...scope(),
        ...bytes(int(0), int(0), int(0), 0)
    ]);
    assert.throws(() => decodeRead(complete), /returned a truncated payload/u);

    // An ordered list whose start does not fit a double: the reader refuses
    // precision loss rather than rounding a value the source wrote.
    assert.throws(
        () => decodeRead(child(6, int(2), long(0x7fffffffffffffffn), 1, 0, int(0))),
        /exceeds JavaScript integer precision/u
    );

    // And the third error code the envelope can carry.
    assert.throws(
        () => decodeRead(Uint8Array.from(bytes("MKC8", 1, int(2), int(1), "x"))),
        (error) => error instanceof ParseError && error.code === "allocationFailed"
    );
});

test("api: a feed reuses the previous read's values for every block that did not move", () => {
    // THE DELTA (#162): a feed's payload names, by position, the previous
    // read's children that the engine retained, and the runtime hands those
    // very values into the new read -- object identity, not a copy -- so a
    // consumer keyed on the previous read finds the same objects. What
    // changed is new: the open paragraph is re-read each time it grows.
    const document = new Document();
    try {
        const first = document.feed("# One\n\npara\n\ntail");
        assert.equal(first.semantic.content.length, 2);
        const second = document.feed(" grows\n\n- item\n");
        assert.equal(second.semantic.content.length, 4);
        assert.equal(second.semantic.content[0], first.semantic.content[0]);
        assert.equal(second.semantic.content[1], first.semantic.content[1]);
        assert.equal(second.semantic.content[2].kind, "paragraph");
        assert.equal(second.semantic.content[3].kind, "list");
        assert.notEqual(second.semantic, first.semantic);
        // The seal is a delta too: everything closed before it is the same
        // value it was, and the read equals the whole-text parse.
        const sealed = document.seal();
        assert.equal(sealed.semantic.content[0], second.semantic.content[0]);
        assert.equal(sealed.semantic.content[1], second.semantic.content[1]);
        assert.equal(sealed.semantic.content[2], second.semantic.content[2]);
        assert.equal(sealed.dump(), new Document("# One\n\npara\n\ntail grows\n\n- item\n").seal().dump());
        // A reused value is still a value: the earlier reads are unchanged.
        assert.equal(first.semantic.content.length, 2);
        assert.equal(second.semantic.content.length, 4);
    } finally {
        document.dispose();
    }
});

test("errors: every refusal the delta reader can make is reached by a payload", () => {
    // A DELTA frame is a tree of ops against the previous read (#162):
    // SPINE (0xfe) rewrites a container's fields and rebuilds its children
    // from ops, SAME (0xff) reuses the next n of the previous node's
    // children, and any other tag is a kind byte opening a whole node. The
    // reader refuses every way the ops can disagree with the previous read,
    // because a delta that landed on the wrong value would be a wrong
    // document rather than an error.
    const int = (value) => ({ int: value });
    const bytes = (...parts) => {
        const out = [];
        for (const part of parts) {
            if (typeof part === "string") out.push(...new TextEncoder().encode(part));
            else if (typeof part === "number") out.push(part & 0xff);
            else for (let shift = 0; shift < 4; shift += 1) out.push((part.int >> (shift * 8)) & 0xff);
        }
        return out;
    };
    const identity = (block, ordinal) => bytes(int(block), int(ordinal));
    const scope = () => bytes(int(1), int(1), int(1), int(1));
    const paragraph = (block, literal) =>
        bytes(
            3,
            ...identity(block, 0),
            ...scope(),
            int(1),
            14,
            ...identity(block, 1),
            ...scope(),
            int(literal.length),
            literal
        );
    // The previous read: a document holding two paragraphs.
    const previous = decodeRead(
        Uint8Array.from([
            ...bytes("MKC8", 0, 0, 1),
            ...identity(1, 0),
            ...scope(),
            ...bytes(int(2)),
            ...paragraph(2, "one"),
            ...paragraph(3, "two")
        ])
    ).semantic;
    const delta = (...ops) => Uint8Array.from([...bytes("MKC8", 0, 1), ...bytes(...ops)]);
    const root = (...ops) => delta(0xfe, 1, ...identity(1, 0), ...scope(), ...bytes(...ops));

    // The healthy shapes: the previous children reused as the same objects,
    // one rewritten as a spine, one written whole, in every mix.
    const same = decodeRead(root(int(1), 0xff, int(2)), previous).semantic;
    assert.equal(same.content.length, 2);
    assert.equal(same.content[0], previous.content[0]);
    assert.equal(same.content[1], previous.content[1]);
    const mixed = decodeRead(
        root(int(3), 0xff, int(1), ...paragraph(3, "changed"), ...paragraph(4, "new")),
        previous
    ).semantic;
    assert.equal(mixed.content.length, 3);
    assert.equal(mixed.content[0], previous.content[0]);
    assert.equal(mixed.content[1].content[0].literal, "changed");
    assert.equal(mixed.content[2].content[0].literal, "new");
    assert.equal(decodeRead(root(int(0)), previous).semantic.content.length, 0);
    // A whole-tree frame ignores the previous read entirely.
    const whole = decodeRead(
        Uint8Array.from([...bytes("MKC8", 0, 0, 1), ...identity(1, 0), ...scope(), ...bytes(int(0))]),
        previous
    ).semantic;
    assert.equal(whole.content.length, 0);

    // A delta that does not open with the document's spine, a spine that
    // renames the kind or the identity, a reuse or a rewrite past the
    // previous node's children, a spine on a node that has no children to
    // address, and an op stream that stops early.
    assert.throws(() => decodeRead(delta(1), previous), /does not open with the document's spine/u);
    assert.throws(() => decodeRead(delta(0xfe, 3), previous), /rewrote a document as a paragraph/u);
    assert.throws(() => decodeRead(delta(0xfe, 1, ...identity(9, 0)), previous), /under another identity/u);
    assert.throws(
        () => decodeRead(root(int(1), 0xff, int(3)), previous),
        /reused children the previous read does not have/u
    );
    assert.throws(
        () => decodeRead(root(int(2), 0xff, int(2), 0xfe), previous),
        /rewrote a child the previous read does not have/u
    );
    assert.throws(() => decodeRead(root(int(1), 0xff, int(-1)), previous), /invalid reused child count -1/u);
    assert.throws(() => decodeRead(root(int(-1)), previous), /invalid op count -1/u);
    assert.throws(
        () => decodeRead(root(int(1), 0xfe, 3, ...identity(2, 0), ...scope(), int(0)), previous),
        /rewrote a paragraph, which has no children to reuse/u
    );
    assert.throws(() => decodeRead(root(int(1), 0xff), previous), /truncated native bridge payload/u);
    assert.throws(() => decodeRead(root(int(1), 0xff, int(1), 0), previous), /returned a truncated payload/u);
});
