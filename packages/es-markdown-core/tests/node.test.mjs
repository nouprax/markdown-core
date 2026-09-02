import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, TreeDumper, visit } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { parseDocumentWithNative } from "../dist/runtime/parser.js";
import { NodeDecoder } from "../dist/wire/node-decoder.js";
import { kindVisitor } from "./visitor.mjs";

test("api: synchronous parse and typed visitor dispatch", () => {
    const document = Document.parse("# Heading\n\nBody\n");
    assert.equal(
        visit(document.content[0], {
            ...kindVisitor,
            visitHeading: (node) => `heading:${node.level}`
        }),
        "heading:1"
    );
    assert.equal(visit(document, kindVisitor), "document");
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
    assert.throws(() => Document.parse("x", 1), TypeError);
    assert.throws(() => Document.parse("x", { tables: "yes" }), TypeError);
    assert.throws(() => Document.parse("x", { tables: null }), TypeError);
});

test("errors: allocation failure is terminal across the WASM boundary", () => {
    let parseCalled = false;
    const allocationFailure = {
        memory: new globalThis.WebAssembly.Memory({ initial: 1 }),
        malloc: () => 0,
        free: () => {},
        es_document_parse: () => {
            parseCalled = true;
            return 0;
        }
    };
    assert.throws(
        () => parseDocumentWithNative(allocationFailure, "text"),
        (error) => error?.name === "ParseError" && error.code === "allocationFailed"
    );
    assert.equal(parseCalled, false, "the runtime must not parse or fall back after allocation refusal");

    const memory = new globalThis.WebAssembly.Memory({ initial: 1 });
    new Uint8Array(memory.buffer, 80, 13).set(new globalThis.TextEncoder().encode("out of memory"));
    const allocations = [8, 32, 40];
    const frees = [];
    const freedErrors = [];
    let documentFreed = false;
    const nativeFailure = {
        memory,
        malloc: () => allocations.shift() ?? 0,
        free: (pointer) => frees.push(pointer),
        es_document_parse: (_source, _length, _flags, errorOutput) => {
            new DataView(memory.buffer).setUint32(errorOutput, 64, true);
            return 0;
        },
        es_document_free: () => {
            documentFreed = true;
        },
        es_document_root: () => {
            throw new Error("a failed parse has no root");
        },
        es_error_code: () => 2,
        es_error_free: (pointer) => freedErrors.push(pointer),
        es_string: (_object, _field, dataOutput, lengthOutput) => {
            const view = new DataView(memory.buffer);
            view.setUint32(dataOutput, 80, true);
            view.setUint32(lengthOutput, 13, true);
            return 1;
        }
    };
    assert.throws(
        () => parseDocumentWithNative(nativeFailure, "text"),
        (error) =>
            error?.name === "ParseError" && error.code === "allocationFailed" && error.message === "out of memory"
    );
    assert.equal(documentFreed, false);
    assert.deepEqual(freedErrors, [64]);
    assert.deepEqual(frees, [40, 32, 8]);
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

test("errors: malformed native values are rejected before they enter the AST", () => {
    // These guards exist because the two sides of the wire are versioned
    // separately -- the Kotlin bridge's wire magic addresses the same hazard -- and
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
        memory: native.memory,
        malloc: () => 8,
        free: () => {},
        es_error_code: () => rawErrorCode,
        es_string: (_object, _field, dataOutput, lengthOutput) => {
            const memory = new DataView(native.memory.buffer);
            memory.setUint32(dataOutput, 0, true);
            memory.setUint32(lengthOutput, 0, true);
            return 0;
        }
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
        memory: native.memory,
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
