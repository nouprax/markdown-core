import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, TreeDumper, visit, walk } from "../dist/index.js";
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

test("api: walking dispatch is typed and preserves owned-field semantics", () => {
    const block = Document.parse(":::note[Title]\nBody\n:::\n").content[0];
    const events = [];
    walk(
        block,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );

    assert.deepEqual(events, [
        "entering:DirectiveBlock",
        "entering:DirectiveLabel",
        "entering:Text",
        "exiting:Text",
        "exiting:DirectiveLabel",
        "entering:Paragraph",
        "entering:Text",
        "exiting:Text",
        "exiting:Paragraph",
        "exiting:DirectiveBlock"
    ]);
    assert.deepEqual(block.content.map(nodeKindName), ["Paragraph"]);

    const table = Document.parse("| a |\n| --- |\n| b |\n").content[0];
    const tableRowKinds = [];
    walk(
        table,
        walkingVisitor((node, phase) => {
            if (phase === "entering" && node.kind === "tableRow") tableRowKinds.push(node.isHeader);
        })
    );
    assert.deepEqual(tableRowKinds, [true, false]);
});

test("api: options gate extensions", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    assert.equal(Document.parse(markdown).content[0].kind, "table");
    assert.equal(Document.parse(markdown, { tables: false }).content[0].kind, "paragraph");
});

test("ast: typed fields are copied from the native result", () => {
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
        es_parse: () => {
            parseCalled = true;
            return 0;
        },
        es_result_free: () => {}
    };
    assert.throws(
        () => parseDocumentWithNative(allocationFailure, "text"),
        (error) => error?.name === "ParseError" && error.code === "allocationFailed"
    );
    assert.equal(parseCalled, false, "the runtime must not parse or fall back after allocation refusal");

    const memory = new globalThis.WebAssembly.Memory({ initial: 1 });
    const result = errorResult(2, "out of memory");
    new Uint8Array(memory.buffer, 64, result.length).set(result);
    const frees = [];
    const freedResults = [];
    const nativeFailure = {
        memory,
        malloc: () => 8,
        free: (pointer) => frees.push(pointer),
        es_parse: () => 64,
        es_result_free: (pointer) => freedResults.push(pointer)
    };
    assert.throws(
        () => parseDocumentWithNative(nativeFailure, "text"),
        (error) =>
            error?.name === "ParseError" && error.code === "allocationFailed" && error.message === "out of memory"
    );
    assert.deepEqual(freedResults, [64]);
    assert.deepEqual(frees, [8]);
});

test("ownership: declarations are readonly without runtime freeze", () => {
    const document = Document.parse("text\n");
    assert.equal(Object.isFrozen(document), false);
    assert.equal(Object.isFrozen(document.content), false);
});

test("robustness: a large document crosses the WASM boundary in one AST result", () => {
    const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    let parseCalls = 0;
    let resultFrees = 0;
    const countedNative = {
        memory: native.memory,
        malloc: native.malloc,
        free: native.free,
        es_parse: (...arguments_) => {
            parseCalls += 1;
            return native.es_parse(...arguments_);
        },
        es_result_free: (result) => {
            resultFrees += 1;
            native.es_result_free(result);
        }
    };
    assert.equal(parseDocumentWithNative(countedNative, unit.repeat(5_000)).content.length, 10_000);
    assert.equal(parseCalls, 1, "AST transfer must be independent of node and field count");
    assert.equal(resultFrees, 1, "the one native result must be released exactly once");
    assert.deepEqual(
        Object.keys(native).filter((name) => name.startsWith("es_node_")),
        [],
        "per-node WASM accessors must not return through the export surface"
    );
});

test("robustness: uncapped list nesting remains traversable", () => {
    // The transfer is an indexed table and the decoder constructs it in
    // reverse order, so depth is data rather than native or JS call-stack use.
    const depth = 10_000;
    const document = Document.parse("- ".repeat(depth) + "leaf\n");
    let entered = 0;
    let exited = 0;
    walk(
        document,
        walkingVisitor((_node, phase) => {
            if (phase === "entering") entered += 1;
            else exited += 1;
        })
    );
    assert.equal(entered, exited);
    assert.ok(entered > depth * 2);

    let node = document.content[0];
    for (let index = 0; index < depth; index += 1) {
        assert.equal(node.kind, "list");
        assert.equal(node.items.length, 1);
        node = node.items[0].content[0];
    }
    assert.equal(node.kind, "paragraph");
});

function walkingVisitor(callback) {
    return Object.fromEntries(Object.keys(kindVisitor).map((method) => [method, callback]));
}

function nodeKindName(node) {
    return node.kind[0].toUpperCase() + node.kind.slice(1);
}

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
    const decoder = new NodeDecoder(new Uint8Array(64));
    assert.throws(() => decoder.referenceForm(9), /invalid reference form 9/u);
    assert.throws(() => decoder.placement(9), /invalid placement mode 9/u);
    assert.throws(() => decoder.listFlavor(9), /invalid list flavor 9/u);
    assert.throws(() => decoder.tableAlignment(9), /invalid table alignment 9/u);
    assert.throws(() => decoder.nullableBoolean(9, "checked"), /invalid checked 9/u);
    assert.equal(decoder.referenceForm(3), "shortcut");
    assert.equal(decoder.placement(2), "standalone");
    assert.equal(decoder.listFlavor(2), "ordered");
    assert.equal(decoder.tableAlignment(0), "none");
    assert.equal(decoder.nullableBoolean(-1, "checked"), null);

    // Native parse failures keep their terminal category across the WASM
    // boundary. In particular, allocation failure must not be collapsed into
    // an internal error that a consumer could mistake for a recoverable path.
    assert.throws(
        () => new NodeDecoder(errorResult(1, "bad")).decodeDocument(),
        (error) => error.code === "invalidArgument"
    );
    assert.throws(
        () => new NodeDecoder(errorResult(2, "out of memory")).decodeDocument(),
        (error) => error.code === "allocationFailed"
    );
    assert.throws(
        () => new NodeDecoder(errorResult(99, "bad")).decodeDocument(),
        (error) => error.code === "internal"
    );

    // A directive label is a typed field with its own node kind. Accepting a
    // generic child here would erase the structural distinction this wire
    // contract exists to preserve.
    const malformedDirective = nativeResult(":note[label]\n");
    const directiveOffset = findNode(malformedDirective, 25);
    const labelIndex = new DataView(malformedDirective.buffer).getUint32(directiveOffset + 32, true);
    const nodesOffset = new DataView(malformedDirective.buffer).getUint32(40, true);
    new DataView(malformedDirective.buffer).setUint32(nodesOffset + labelIndex * 96, 3, true);
    assert.throws(
        () => new NodeDecoder(malformedDirective).decodeDocument(),
        /directive label field contains a non-label node/u
    );

    const unknownKind = nativeResult("text\n");
    new DataView(unknownKind.buffer).setUint32(findNode(unknownKind, 14), 99, true);
    assert.throws(() => new NodeDecoder(unknownKind).decodeDocument(), /unknown node kind 99/u);

    const badMagic = nativeResult("text\n");
    badMagic[0] = 0;
    assert.throws(() => new NodeDecoder(badMagic).decodeDocument(), /invalid native result/u);
});

function errorResult(code, message) {
    const encoded = new globalThis.TextEncoder().encode(message);
    const result = new Uint8Array(64 + encoded.length);
    const view = new DataView(result.buffer);
    result.set([0x4d, 0x43, 0x42, 0x31]);
    view.setUint32(4, result.length, true);
    view.setUint32(8, 1, true);
    view.setInt32(12, code, true);
    view.setUint32(16, 64, true);
    view.setUint32(20, encoded.length, true);
    result.set(encoded, 64);
    return result;
}

function nativeResult(source) {
    const encoded = new globalThis.TextEncoder().encode(source);
    const sourcePointer = native.malloc(Math.max(encoded.length, 1));
    assert.notEqual(sourcePointer, 0);
    let resultPointer = 0;
    try {
        new Uint8Array(native.memory.buffer, sourcePointer, encoded.length).set(encoded);
        resultPointer = native.es_parse(sourcePointer, encoded.length, 0x1ff);
        assert.notEqual(resultPointer, 0);
        const length = new DataView(native.memory.buffer).getUint32(resultPointer + 4, true);
        return Uint8Array.from(new Uint8Array(native.memory.buffer, resultPointer, length));
    } finally {
        if (resultPointer) native.es_result_free(resultPointer);
        native.free(sourcePointer);
    }
}

function findNode(result, kind) {
    const view = new DataView(result.buffer, result.byteOffset, result.byteLength);
    const count = view.getUint32(24, true);
    const nodesOffset = view.getUint32(40, true);
    for (let index = 0; index < count; index += 1) {
        const offset = nodesOffset + index * 96;
        if (view.getUint32(offset, true) === kind) return offset;
    }
    throw new Error(`result does not contain kind ${kind}`);
}
