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

test("api: the dialect has no switches, so a plain parse recognizes every feature", () => {
    // One witness per feature that used to sit behind a `ParseOptions`
    // field, and one for the substitution smart punctuation used to make.
    assert.equal(Document.parse("| a |\n| --- |\n| b |\n").content[0].kind, "table");
    for (const [source, witness] of [
        ["~~x~~\n", "Strikethrough scope="],
        ["www.example.com\n", "Link scope="],
        ["- [x] task\n", "checked=true"],
        ["ref[^a]\n\n[^a]: note\n", "FootnoteReference scope="],
        ["$x$\n", "Formula scope="],
        [":badge[label]\n", "Directive scope="],
        ['"quotes" -- ...\n', 'literal="\\"quotes\\" -- ..."']
    ]) {
        assert.ok(Document.parse(source).dump().includes(witness), `${witness} for ${JSON.stringify(source)}`);
    }
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

test("references: every occurrence of one definition crosses the boundary once and is materialized once", () => {
    // M2: the C tree shares one resource across every occurrence of a
    // definition; the Wasm result writes its strings once and each later
    // occurrence points at the same bytes, and the decoder reuses the value it
    // built for the first. The string blob therefore stays near the source
    // size where copying would multiply the destination by the occurrences.
    const destination = `/${"u".repeat(1024)}`;
    const count = 20_000;
    const source = `[a]: ${destination}\n\n${"[a]\n\n".repeat(count)}`;
    let stringsLength = -1;
    const measuringNative = {
        memory: native.memory,
        malloc: native.malloc,
        free: native.free,
        es_parse: (...arguments_) => {
            const result = native.es_parse(...arguments_);
            stringsLength = new DataView(native.memory.buffer).getUint32(result + 60, true);
            return result;
        },
        es_result_free: native.es_result_free
    };
    const document = parseDocumentWithNative(measuringNative, source);
    const links = document.content.map((paragraph) => paragraph.content[0]);
    assert.equal(links.length, count);
    assert.deepEqual(links[0].dest, { kind: "url", value: destination });
    assert.ok(
        links.every((link) => link.kind === "link" && link.dest === links[0].dest),
        "every occurrence materializes the one destination"
    );
    assert.ok(
        stringsLength >= 0 && stringsLength < 2 * source.length,
        `the string blob holds ${stringsLength} bytes for ${source.length} source bytes`
    );
});

test("callouts: every `>` container is a metadata-free callout", () => {
    // M3: the kind is `callout`; the metadata rule that fills variant, fold,
    // and title in lands with O8, so every callout reads as metadata-free and
    // dumps its fields as such.
    const document = Document.parse("> quote\n");
    const [callout] = document.content;
    assert.equal(callout.kind, "callout");
    assert.equal(callout.variant, null);
    assert.equal(callout.fold, "none");
    assert.equal(callout.title, null);
    assert.equal(callout.content.length, 1);
    assert.equal(
        document.dump(),
        "Document scope=1:1..1:7 children=1\n" +
            "└── Callout scope=1:1..1:7 variant=null fold=none children=1\n" +
            "    └── Paragraph scope=1:3..1:7 children=1\n" +
            '        └── Text scope=1:3..1:7 literal="quote" children=0\n'
    );
});

test("callouts: a title is decoded from the auxiliary range before the content and dumped as a group", () => {
    // The title path of the wire: a node-valued list the record owns through
    // its auxiliary range under flag 1. No parse produces one until O8, so the
    // result is built by hand: a document holding one expanded `note` callout
    // whose title is the text `T` and whose content is empty.
    const nodeSize = 96;
    const strings = Uint8Array.from("noteT", (character) => character.charCodeAt(0));
    const nodesOffset = 64;
    const edgesOffset = nodesOffset + 3 * nodeSize;
    const stringsOffset = edgesOffset + 2 * 4;
    const total = stringsOffset + strings.length;
    const bytes = new Uint8Array(total);
    const view = new DataView(bytes.buffer);
    bytes.set([0x4d, 0x43, 0x42, 0x31], 0);
    for (const [offset, value] of [
        [4, total],
        [24, 3],
        [28, 2],
        [40, nodesOffset],
        [44, edgesOffset],
        [48, stringsOffset],
        [52, stringsOffset],
        [56, stringsOffset],
        [60, strings.length]
    ]) {
        view.setUint32(offset, value, true);
    }
    const node = (index, kind, scope, fields) => {
        const at = nodesOffset + index * nodeSize;
        view.setUint32(at, kind, true);
        for (const [slot, value] of scope.entries()) view.setInt32(at + 8 + slot * 4, value, true);
        view.setUint32(at + 32, 0xffff_ffff, true);
        view.setUint32(at + 36, 0xffff_ffff, true);
        for (let slot = 0; slot < 4; ++slot) view.setUint32(at + 64 + slot * 8, 0xffff_ffff, true);
        for (const [offset, value] of Object.entries(fields)) view.setUint32(at + Number(offset), value, true);
    };
    node(0, 1, [1, 1, 1, 8], { 24: 0, 28: 1 });
    node(1, 2, [1, 1, 1, 8], { 4: 1, 24: 1, 28: 0, 36: 1, 40: 1, 44: 2, 64: stringsOffset, 68: 4 });
    node(2, 14, [1, 10, 1, 10], { 64: stringsOffset + 4, 68: 1 });
    view.setUint32(edgesOffset, 1, true);
    view.setUint32(edgesOffset + 4, 2, true);
    bytes.set(strings, stringsOffset);

    const document = new NodeDecoder(bytes).decodeDocument();
    const [callout] = document.content;
    assert.equal(callout.kind, "callout");
    assert.equal(callout.variant, "note");
    assert.equal(callout.fold, "expanded");
    assert.deepEqual(
        callout.title.map((child) => [child.kind, child.literal]),
        [["text", "T"]]
    );
    assert.deepEqual(callout.content, []);
    assert.equal(
        TreeDumper.dump(document),
        "Document scope=1:1..1:8 children=1\n" +
            '└── Callout scope=1:1..1:8 variant="note" fold=expanded children=0\n' +
            "    └── Title children=1\n" +
            '        └── Text scope=1:10..1:10 literal="T" children=0\n'
    );
    const events = [];
    walk(
        callout,
        walkingVisitor((visited, phase) => events.push(`${phase}:${nodeKindName(visited)}`))
    );
    assert.deepEqual(events, ["entering:Callout", "entering:Text", "exiting:Text", "exiting:Callout"]);
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
    assert.deepEqual(link.dest, { kind: "url", value: "/u" });
    assert.equal(link.title, "t");
});

test("ast: the decoder's reference, formula, list and empty-string arms are exercised", () => {
    // Decoder arms that no other suite reaches, and each is an ordinary
    // language feature rather than a defensive branch: a resolved reference's
    // shared resource, a formula's placement, an ordered list's flavour, and
    // requirement 14's "written and empty" answer, which is the one a `null`
    // would be confused with.
    const document = Document.parse(
        ['[foo]: /url "t"', "", "See [foo] and [x][foo] and $$x$$ and [a]().", "", "3. one", "4. two", ""].join("\n")
    );

    // M2: the definition produces no node, and each reference is the link it
    // names, with the definition's destination and title. Both occurrences
    // read one resource, materialized once.
    const [paragraph, list] = document.content;
    const references = paragraph.content.filter((node) => node.kind === "link" && node.dest.value === "/url");
    assert.equal(references.length, 2);
    assert.deepEqual(references[0].dest, { kind: "url", value: "/url" });
    assert.equal(references[0].title, "t");
    assert.equal(references[0].dest, references[1].dest, "one definition materializes one destination");

    const formula = paragraph.content.find((node) => node.kind === "formula");
    assert.equal(formula.mode, "standalone");
    assert.equal(formula.literal, "x");

    // `[a]()` WROTE a destination and wrote nothing in it. Empty is not absent:
    // the tagged value is the `url` branch holding the empty string. It is the
    // paragraph's third link, after the two resolved references.
    const links = paragraph.content.filter((node) => node.kind === "link");
    assert.equal(links.length, 3);
    assert.deepEqual(links[2].dest, { kind: "url", value: "" });
    assert.equal(links[2].title, null);

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
    assert.throws(() => decoder.placement(9), /invalid placement mode 9/u);
    assert.throws(() => decoder.listFlavor(9), /invalid list flavor 9/u);
    assert.throws(() => decoder.tableAlignment(9), /invalid table alignment 9/u);
    assert.throws(() => decoder.nullableBoolean(9, "checked"), /invalid checked 9/u);
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
        resultPointer = native.es_parse(sourcePointer, encoded.length);
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
