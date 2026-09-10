import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, TreeDumper, visit, walk } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { parseDocumentWithNative } from "../dist/runtime/parser.js";
import { kinds } from "../dist/wire/kinds.js";
import { NodeDecoder } from "../dist/wire/node-decoder.js";
import { kindVisitor } from "./visitor.mjs";

test("ast: dimensions belong to each image occurrence while its destination stays shared", () => {
    const document = Document.parse('![*alt*|2147483647x2][r] ![3][r] ![bad|01][r]\n\n[r]: /shared "title"\n');
    const images = document.content[0].content.filter((node) => node.kind === "media");
    assert.deepEqual(
        images.map((node) => node.dimensions),
        [{ width: 2147483647, height: 2 }, { width: 3, height: null }, null]
    );
    assert.equal(images[0].dest, images[1].dest);
    assert.equal(images[1].dest, images[2].dest);
    assert.equal(images[0].title, "title");
    assert.equal(images[0].content[0].kind, "emphasis");
    assert.equal(images[0].content[0].content[0].literal, "alt");
    assert.equal(images[0].content[0].scope.end.column, 7);
    assert.deepEqual(images[1].content, []);
    assert.equal(images[2].content[0].literal, "bad|01");
    const events = [];
    walk(
        images[0],
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "entering:Media",
        "entering:Emphasis",
        "entering:Text",
        "exiting:Text",
        "exiting:Emphasis",
        "exiting:Media"
    ]);
});

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
            if (phase === "entering" && node.kind === "tableRow") tableRowKinds.push(node.scope.start.line);
        })
    );
    assert.deepEqual(tableRowKinds, [1, 3]);
});

test("ast: marks retain typed content and walk both phases after native release", () => {
    const mark = Document.parse("==a *b*==").content[0].content[0];
    assert.equal(visit(mark, { ...kindVisitor, visitMark: (node) => node.content.length }), 2);
    const events = [];
    walk(
        mark,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "entering:mark",
        "entering:text",
        "exiting:text",
        "entering:emphasis",
        "entering:text",
        "exiting:text",
        "exiting:emphasis",
        "exiting:mark"
    ]);
    assert.equal(mark.content[1].content[0].literal, "b");
    assert.deepEqual(mark.scope, { start: { line: 1, column: 1 }, end: { line: 1, column: 9 } });
});

test("ast: insertions retain typed content and walk both phases after native release", () => {
    const insertion = Document.parse("++a *b*++").content[0].content[0];
    assert.equal(visit(insertion, { ...kindVisitor, visitInsertion: (node) => node.content.length }), 2);
    const events = [];
    walk(
        insertion,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "entering:insertion",
        "entering:text",
        "exiting:text",
        "entering:emphasis",
        "entering:text",
        "exiting:text",
        "exiting:emphasis",
        "exiting:insertion"
    ]);
    assert.equal(insertion.content[1].content[0].literal, "b");
    assert.deepEqual(insertion.scope, { start: { line: 1, column: 1 }, end: { line: 1, column: 9 } });
});

test("ast: spans retain typed content and walk both phases after native release", () => {
    const span = Document.parse("[a *b*]{}").content[0].content[0];
    assert.equal(visit(span, { ...kindVisitor, visitSpan: (node) => node.content.length }), 2);
    const events = [];
    walk(
        span,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "entering:span",
        "entering:text",
        "exiting:text",
        "entering:emphasis",
        "entering:text",
        "exiting:text",
        "exiting:emphasis",
        "exiting:span"
    ]);
    assert.equal(span.content[1].content[0].literal, "b");
    assert.deepEqual(span.scope, { start: { line: 1, column: 1 }, end: { line: 1, column: 9 } });
});

test("ast: superscripts retain typed content and walk both phases after native release", () => {
    const superscript = Document.parse("^a*b*^").content[0].content[0];
    assert.equal(visit(superscript, { ...kindVisitor, visitSuperscript: (node) => node.content.length }), 2);
    const events = [];
    walk(
        superscript,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "entering:superscript",
        "entering:text",
        "exiting:text",
        "entering:emphasis",
        "entering:text",
        "exiting:text",
        "exiting:emphasis",
        "exiting:superscript"
    ]);
    assert.equal(superscript.content[1].content[0].literal, "b");
    assert.deepEqual(superscript.scope, { start: { line: 1, column: 1 }, end: { line: 1, column: 6 } });
});

test("ast: subscripts retain typed content and walk both phases after native release", () => {
    const subscript = Document.parse("~a*b*~").content[0].content[0];
    assert.equal(visit(subscript, { ...kindVisitor, visitSubscript: (node) => node.content.length }), 2);
    const events = [];
    walk(
        subscript,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "entering:subscript",
        "entering:text",
        "exiting:text",
        "entering:emphasis",
        "entering:text",
        "exiting:text",
        "exiting:emphasis",
        "exiting:subscript"
    ]);
    assert.equal(subscript.content[1].content[0].literal, "b");
    assert.deepEqual(subscript.scope, { start: { line: 1, column: 1 }, end: { line: 1, column: 6 } });
});

test("api: the dialect has no switches, so a plain parse recognizes every feature", () => {
    // One witness per feature that used to sit behind a `ParseOptions`
    // field, and one for the substitution smart punctuation used to make.
    assert.equal(Document.parse("| a |\n| --- |\n| b |\n").content[0].kind, "table");
    for (const [source, witness] of [
        ["~~x~~\n", "Strikethrough scope="],
        ["www.example.com\n", "Link scope="],
        ["- [x] task\n", 'marker="x"'],
        ["ref[^a]\n\n[^a]: note\n", "Cite scope="],
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
    assert.deepEqual(
        document.content[1].columns.map((column) => column.alignment),
        ["center"]
    );
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

test("ownership: every occurrence of one definition crosses the boundary once and is materialized once", () => {
    // M2: the C tree shares one resource across every occurrence of a
    // definition; the Wasm result writes its strings once and each later
    // occurrence points at the same bytes, and the decoder reuses the value it
    // built for the first. The string blob therefore stays near the source
    // size where copying would multiply the destination by the occurrences.
    const destination = `/${"u".repeat(1024)}`;
    const count = 20_000;
    const anchor = "a".repeat(1024);
    const classes = Array.from({ length: 1024 }, () => "c");
    const source = `[a]: ${destination} {#${anchor} ${classes.map((value) => `.${value}`).join(" ")} k=${destination}}\n\n${"[a]\n\n".repeat(count)}`;
    let stringsLength = -1;
    let attributeCount = -1;
    const measuringNative = {
        memory: native.memory,
        malloc: native.malloc,
        free: native.free,
        es_parse: (...arguments_) => {
            const result = native.es_parse(...arguments_);
            stringsLength = new DataView(native.memory.buffer).getUint32(result + 60, true);
            attributeCount = new DataView(native.memory.buffer).getUint32(result + 32, true);
            return result;
        },
        es_result_free: native.es_result_free
    };
    const document = parseDocumentWithNative(measuringNative, source);
    const links = document.content.map((paragraph) => paragraph.content[0]);
    assert.equal(links.length, count);
    assert.equal(links[0].anchor, anchor);
    assert.deepEqual(links[0].attributes.classes, classes);
    assert.equal(attributeCount, classes.length + 1, "definition attributes cross Wasm once");
    assert.throws(() => {
        links[0].attributes.classes[0] = "edited";
    }, TypeError);
    assert.throws(() => {
        links[0].attributes.records[0].value = "edited";
    }, TypeError);
    assert.throws(() => {
        links[0].attributes.records = [];
    }, TypeError);
    assert.equal(links[1].attributes.classes[0], "c");
    assert.equal(links[1].attributes.records[0].value, destination);
    assert.ok(links.every((link) => link.attributes.classes === links[0].attributes.classes));
    assert.ok(links.every((link) => link.attributes.records === links[0].attributes.records));
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

test("ownership: forward heading references share their finalized target without inheriting heading attributes", () => {
    const anchor = "a".repeat(1024);
    const count = 5_000;
    const source = `${"[Target]\n\n".repeat(count)}# Target {#${anchor} .heading k=1}\n`;
    let stringsLength = -1;
    const measuringNative = {
        ...native,
        es_parse: (...arguments_) => {
            const result = native.es_parse(...arguments_);
            stringsLength = new DataView(native.memory.buffer).getUint32(result + 60, true);
            return result;
        }
    };
    const document = parseDocumentWithNative(measuringNative, source);
    const links = document.content.slice(0, count).map((paragraph) => paragraph.content[0]);
    assert.equal(document.content[count].anchor, anchor);
    assert.deepEqual(links[0].dest, { kind: "url", value: `#${anchor}` });
    assert.ok(links.every((link) => link.dest === links[0].dest));
    assert.ok(links.every((link) => link.anchor === null && link.title === null));
    assert.deepEqual(links[0].attributes, { classes: [], records: [] });
    assert.ok(stringsLength >= 0 && stringsLength < 2 * source.length);
});

test("ast: an ordinary quote is a metadata-free callout", () => {
    const document = Document.parse("> quote\n");
    const [callout] = document.content;
    assert.equal(callout.kind, "callout");
    assert.equal(callout.variant, null);
    assert.equal(callout.collapsed, null);
    assert.equal(callout.title, null);
    assert.equal(callout.content.length, 1);
    assert.equal(
        document.dump(),
        "Document scope=1:1..1:7 anchor=null attributes={} children=1\n" +
            "└── Callout scope=1:1..1:7 anchor=null attributes={} variant=null collapsed=null children=1\n" +
            "    └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1\n" +
            '        └── Text scope=1:3..1:7 anchor=null attributes={} literal="quote" children=0\n'
    );
});

test("ast: authored callout title walks before body after native release", () => {
    const callout = Document.parse("> [!CuStOm]- **T**\n> body\n").content[0];
    assert.equal(callout.variant, "CuStOm");
    assert.equal(callout.collapsed, true);
    assert.deepEqual(callout.title.map(nodeKindName), ["Strong"]);
    assert.deepEqual(callout.content.map(nodeKindName), ["Paragraph"]);
    assert.equal(callout.title[0].content[0].literal, "T");
    const events = [];
    walk(
        callout,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "entering:Callout",
        "entering:Strong",
        "entering:Text",
        "exiting:Text",
        "exiting:Strong",
        "entering:Paragraph",
        "entering:Text",
        "exiting:Text",
        "exiting:Paragraph",
        "exiting:Callout"
    ]);
    const [comment, empty] = Document.parse("> [!note]+ %%t%%\n\n> [!note]\n").content;
    assert.equal(comment.collapsed, false);
    assert.equal(comment.title[0].kind, "comment");
    assert.equal(comment.title[0].literal, "t");
    assert.deepEqual(comment.content, []);
    assert.equal(empty.title, null);
    assert.equal(empty.collapsed, null);
});

test("ast: a title is decoded from the auxiliary range before the content and dumped as a group", () => {
    // The title path of the wire: a node-valued list the record owns through
    // its auxiliary range. This transport fixture is built by hand:
    // a document holding one collapsed `note` callout whose
    // title is the text `T` and whose content is empty.
    const nodeSize = 160;
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
        view.setUint32(at + 96, 0xffff_ffff, true);
        view.setUint32(at + 120, 0xffff_ffff, true);
        view.setUint32(at + 32, 0xffff_ffff, true);
        view.setUint32(at + 36, 0xffff_ffff, true);
        for (let slot = 0; slot < 4; ++slot) view.setUint32(at + 64 + slot * 8, 0xffff_ffff, true);
        for (const [offset, value] of Object.entries(fields)) view.setUint32(at + Number(offset), value, true);
    };
    node(0, 1, [1, 1, 1, 8], { 24: 0, 28: 1 });
    node(1, 2, [1, 1, 1, 8], { 24: 1, 28: 0, 36: 1, 40: 1, 44: 1, 64: stringsOffset, 68: 4 });
    node(2, 13, [1, 10, 1, 10], { 64: stringsOffset + 4, 68: 1 });
    view.setUint32(edgesOffset, 1, true);
    view.setUint32(edgesOffset + 4, 2, true);
    bytes.set(strings, stringsOffset);

    const document = new NodeDecoder(bytes).decodeDocument();
    const [callout] = document.content;
    assert.equal(callout.kind, "callout");
    assert.equal(callout.variant, "note");
    assert.equal(callout.collapsed, true);
    assert.deepEqual(
        callout.title.map((child) => [child.kind, child.literal]),
        [["text", "T"]]
    );
    assert.deepEqual(callout.content, []);
    assert.equal(
        TreeDumper.dump(document),
        "Document scope=1:1..1:8 anchor=null attributes={} children=1\n" +
            '└── Callout scope=1:1..1:8 anchor=null attributes={} variant="note" collapsed=true children=0\n' +
            "    └── Title children=1\n" +
            '        └── Text scope=1:10..1:10 anchor=null attributes={} literal="T" children=0\n'
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
    return {
        ...Object.fromEntries(Object.keys(kindVisitor).map((method) => [method, callback])),
        // The scoped values have no `kind`; their callbacks report their names.
        visitCitation: (value, phase) => callback({ kind: "citation", ...value }, phase),
        visitSpecimen: (value, phase) => callback({ kind: "specimen", ...value }, phase),
        visitFootnote: (value, phase) => callback({ kind: "footnote", ...value }, phase)
    };
}

function nodeKindName(node) {
    return node.kind[0].toUpperCase() + node.kind.slice(1);
}

test("ast: inline footnotes keep direct content, source ids and finite visitation", () => {
    const document = Document.parse("^[^[x]]\n\n[^inline-1]: authored\n");
    assert.deepEqual(
        document.footnotes.map((note) => note.id),
        ["inline-1-1", "inline-2", "inline-1"]
    );
    assert.deepEqual(
        document.footnotes.map((note) => note.content[0].kind),
        ["cite", "text", "paragraph"]
    );
    const outer = document.content[0].content[0];
    assert.deepEqual(outer.citations[0].referent, { kind: "footnote", id: "inline-1-1" });
    assert.deepEqual(document.footnotes[0].content[0].citations[0].referent, { kind: "footnote", id: "inline-2" });
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "entering:Document",
        "entering:Paragraph",
        "entering:Cite",
        "entering:Citation",
        "exiting:Citation",
        "exiting:Cite",
        "exiting:Paragraph",
        "entering:Footnote",
        "entering:Cite",
        "entering:Citation",
        "exiting:Citation",
        "exiting:Cite",
        "exiting:Footnote",
        "entering:Footnote",
        "entering:Text",
        "exiting:Text",
        "exiting:Footnote",
        "entering:Footnote",
        "entering:Paragraph",
        "entering:Text",
        "exiting:Text",
        "exiting:Paragraph",
        "exiting:Footnote",
        "exiting:Document"
    ]);
});

test("ast: an inherited call is a one-item cite and the document owns its footnotes", () => {
    // M4: repeated calls share one footnote; the item names it by id with
    // empty affixes; the footnote is a document-owned value after the
    // content, never a child; the walk visits values through their own
    // callbacks, the footnotes after the content.
    const document = Document.parse("[^a] [^a]\n\n[^a]: once\n");
    const [paragraph] = document.content;
    const cite = paragraph.content[0];
    assert.equal(cite.kind, "cite");
    assert.equal(cite.citations.length, 1);
    assert.deepEqual(cite.citations[0].referent, { kind: "footnote", id: "a" });
    assert.deepEqual(cite.citations[0].prefix, []);
    assert.deepEqual(cite.citations[0].suffix, []);
    assert.deepEqual(cite.citations[0].scope, { start: { line: 1, column: 2 }, end: { line: 1, column: 3 } });
    assert.equal(document.content.length, 1);
    assert.equal(document.footnotes.length, 1);
    assert.equal(document.footnotes[0].id, "a");
    assert.equal(document.footnotes[0].content[0].kind, "paragraph");
    assert.equal(
        document.dump(),
        "Document scope=1:1..3:10 anchor=null attributes={} children=1\n" +
            "├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=3\n" +
            "│   ├── Cite scope=1:1..1:4 anchor=null attributes={} children=1\n" +
            '│   │   └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0\n' +
            "│   │       ├── CitationPrefix children=0\n" +
            "│   │       └── CitationSuffix children=0\n" +
            '│   ├── Text scope=1:5..1:5 anchor=null attributes={} literal=" " children=0\n' +
            "│   └── Cite scope=1:6..1:9 anchor=null attributes={} children=1\n" +
            '│       └── Citation scope=1:7..1:8 referent=footnote(id="a") children=0\n' +
            "│           ├── CitationPrefix children=0\n" +
            "│           └── CitationSuffix children=0\n" +
            '└── Footnote scope=3:1..3:10 id="a" children=1\n' +
            "    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1\n" +
            '        └── Text scope=3:7..3:10 anchor=null attributes={} literal="once" children=0\n'
    );
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "entering:Document",
        "entering:Paragraph",
        "entering:Cite",
        "entering:Citation",
        "exiting:Citation",
        "exiting:Cite",
        "entering:Text",
        "exiting:Text",
        "entering:Cite",
        "entering:Citation",
        "exiting:Citation",
        "exiting:Cite",
        "exiting:Paragraph",
        "entering:Footnote",
        "entering:Paragraph",
        "entering:Text",
        "exiting:Text",
        "exiting:Paragraph",
        "exiting:Footnote",
        "exiting:Document"
    ]);
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
    const directiveOffset = findNode(malformedDirective, kinds.indexOf("directive"));
    const labelIndex = new DataView(malformedDirective.buffer).getUint32(directiveOffset + 32, true);
    const nodesOffset = new DataView(malformedDirective.buffer).getUint32(40, true);
    new DataView(malformedDirective.buffer).setUint32(nodesOffset + labelIndex * 160, 3, true);
    assert.throws(
        () => new NodeDecoder(malformedDirective).decodeDocument(),
        /directive label field contains a non-label node/u
    );

    const unknownKind = nativeResult("text\n");
    new DataView(unknownKind.buffer).setUint32(findNode(unknownKind, kinds.indexOf("text")), 99, true);
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

test("ast: every ordered delimiter and associated numbering value survives decoding", () => {
    const delimiters = [
        [1, false, "period"],
        [2, false, { kind: "parenthesis", closed: false }],
        [2, true, { kind: "parenthesis", closed: true }],
        [3, false, "default"]
    ];
    for (const [variantRaw, kind] of [
        [2, "alpha"],
        [3, "roman"]
    ]) {
        for (const lowercased of [false, true]) {
            for (const [delimiterRaw, closed, expected] of delimiters) {
                const bytes = nativeResult("1. item\n");
                const view = new DataView(bytes.buffer);
                const at = findNode(bytes, kinds.indexOf("list")) + 4;
                const flags = view.getUint32(at, true) & ~0x3fc;
                view.setUint32(
                    at,
                    flags | (variantRaw << 2) | (delimiterRaw << 5) | (Number(closed) << 8) | (Number(lowercased) << 9),
                    true
                );
                const document = new NodeDecoder(bytes).decodeDocument();
                assert.deepEqual(document.content[0].variant, { kind, lowercased });
                assert.deepEqual(document.content[0].delimiter, expected);
                assert.match(TreeDumper.dump(document), new RegExp(`variant=${kind}\\(lowercased=${lowercased}\\)`));
                const spelling = typeof expected === "string" ? expected : `parenthesis(closed=${closed})`;
                assert.ok(TreeDumper.dump(document).includes(`delimiter=${spelling}`));
            }
        }
    }
    const malformed = nativeResult("1. item\n");
    const view = new DataView(malformed.buffer);
    const at = findNode(malformed, kinds.indexOf("list")) + 4;
    view.setUint32(at, view.getUint32(at, true) | (7 << 5), true);
    assert.throws(() => new NodeDecoder(malformed).decodeDocument(), /invalid ordered list facts/u);
});

test("ast: a UTF-8 task marker is an owned string, independent of the payload", () => {
    const bytes = nativeResult("- [x] 🚀\n");
    const view = new DataView(bytes.buffer);
    const text = findNode(bytes, kinds.indexOf("text"));
    const item = findNode(bytes, kinds.indexOf("listItem"));
    view.setUint32(item + 64, view.getUint32(text + 64, true), true);
    view.setUint32(item + 68, view.getUint32(text + 68, true), true);
    const document = new NodeDecoder(bytes).decodeDocument();
    bytes.fill(0);
    assert.equal(document.content[0].items[0].marker, "🚀");
    assert.ok(TreeDumper.dump(document).includes('marker="🚀"'));
});

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
        const offset = nodesOffset + index * 160;
        if (view.getUint32(offset, true) === kind) return offset;
    }
    throw new Error(`result does not contain kind ${kind}`);
}

test("ast: specimen definitions and references retain ownership, nulls and reset facts", () => {
    // Parsing these values lands with P9b. Existing definition records supply
    // the shared topology; only the reserved value tags and scalar facts change.
    const bytes = nativeResult("[^note] [^étude]\n\n[^note]: note\n\n[^étude]: body\n\n[^anonymous]: tail\n");
    const view = new DataView(bytes.buffer);
    const nodes = view.getUint32(40, true);
    let definitions = 0;
    let citations = 0;
    let firstSpecimen;
    for (let i = 0; i < view.getUint32(24, true); ++i) {
        const at = nodes + i * 160;
        const kind = view.getUint32(at, true);
        if (kind === 0x100 && ++citations === 2) view.setInt32(at + 44, 3, true);
        if (kind === 0x101 && ++definitions > 1) {
            view.setUint32(at, 0x102, true);
            if (definitions === 2) {
                firstSpecimen = at;
                view.setUint32(at + 4, 1, true);
                view.setBigInt64(at + 56, 5n, true);
            } else {
                view.setUint32(at + 64, 0xffff_ffff, true);
                view.setUint32(at + 68, 0, true);
            }
        }
    }
    const document = new NodeDecoder(bytes).decodeDocument();
    const invalid = bytes.slice();
    new DataView(invalid.buffer).setBigInt64(firstSpecimen + 56, 9007199254740993n, true);
    assert.throws(() => new NodeDecoder(invalid).decodeDocument(), /precision/);
    bytes.fill(0);
    assert.equal(document.footnotes.length, 1);
    assert.deepEqual(
        document.specimens.map(({ id, start }) => [id, start]),
        [
            ["étude", 5],
            [null, null]
        ]
    );
    assert.equal(document.specimens[0].content[0].content[0].literal, "body");
    assert.deepEqual(document.content[0].content[2].citations[0].referent, { kind: "specimen", id: "étude" });
    const dumped = document.dump();
    assert.match(dumped, /Specimen scope=5:1..6:0 id="étude" start=5 children=1/);
    assert.match(dumped, /Specimen scope=7:1..7:18 id=null start=null children=1/);
    assert.ok(dumped.indexOf("Footnote scope=") < dumped.indexOf("Specimen scope="));
    const events = [];
    walk(
        document,
        walkingVisitor((value, phase) => {
            if (phase === "entering") events.push(value.kind);
        })
    );
    assert.equal(events.filter((kind) => kind === "specimen").length, 2);
    assert.ok(events.indexOf("footnote") < events.indexOf("specimen"));
});

test("ast: table groups, column widths and spans survive the wire as owned facts", () => {
    const source = "| h | i |\n| - | - |\n| b | c |\n| f | g |\n";
    const bytes = nativeResult(source);
    const view = new DataView(bytes.buffer);
    const table = findNode(bytes, kinds.indexOf("table"));
    // The third authored row becomes the foot group; no row-local tag exists.
    view.setBigInt64(table + 48, 1n, true);
    view.setBigInt64(table + 56, 1n, true);
    const column = view.getUint32(52, true);
    view.setUint32(column + 4, 1, true);
    view.setFloat64(column + 8, 0.1, true);
    const document = new NodeDecoder(bytes).decodeDocument();
    const value = document.content[0];
    assert.equal(value.head[0].cells[0].content[0].literal, "h");
    assert.equal(value.content[0].cells[0].content[0].literal, "b");
    assert.equal(value.foot[0].cells[0].content[0].literal, "f");
    assert.deepEqual(value.columns, [
        { alignment: "none", relative: 0.1 },
        { alignment: "none", relative: null }
    ]);
    assert.ok(!("isHeader" in value.head[0]));
    const visited = [];
    walk(
        value,
        walkingVisitor((node, phase) => {
            if (phase === "entering" && node.kind === "text") visited.push(node.literal);
        })
    );
    assert.deepEqual(visited, ["h", "i", "b", "c", "f", "g"]);
    assert.match(value.dump(), /columns=\[none:0.1,none:null\] children=3/);
    assert.match(value.dump(), /TableFoot children=1/);
    const row = findNode(bytes, kinds.indexOf("tableCell"));
    const malformed = (change, pattern) => {
        const copy = bytes.slice();
        change(new DataView(copy.buffer));
        assert.throws(() => new NodeDecoder(copy).decodeDocument(), pattern);
    };
    malformed((v) => v.setInt32(table + 44, -1, true), /row groups/);
    malformed((v) => v.setBigInt64(table + 56, 2n, true), /row groups/);
    malformed((v) => v.setBigInt64(row + 56, 0n, true), /spans/);
    malformed((v) => v.setBigInt64(row + 48, -1n, true), /spans/);
    malformed((v) => v.setFloat64(column + 8, Number.NaN, true), /column width/);
    malformed((v) => v.setFloat64(column + 8, 0, true), /column width/);
    malformed((v) => v.setUint32(column + 4, 2, true), /presence/);
    bytes.fill(0);
    assert.equal(value.columns[0].relative, 0.1);
    assert.equal(value.foot[0].cells[0].content[0].literal, "f");
});

test("ast: metadata preserves tags, decimal text, duplicate keys and owned lists", () => {
    const strings = [
        "key",
        "n",
        "s",
        "empty",
        "list",
        "9007199254740993",
        "中文\nquoted",
        "number",
        "text",
        "1.25",
        ""
    ];
    const encoded = strings.map((value) => new globalThis.TextEncoder().encode(value));
    const nodes = 64,
        edges = nodes + 8 * 160,
        attributes = edges + 6 * 4,
        blob = attributes + 2 * 16;
    const bytes = new Uint8Array(blob + encoded.reduce((n, value) => n + value.length, 0));
    const view = new DataView(bytes.buffer);
    const put = (offset, value) => view.setUint32(offset, value, true);
    bytes.set([0x4d, 0x43, 0x42, 0x31]);
    for (const [offset, value] of [
        [4, bytes.length],
        [24, 8],
        [28, 6],
        [32, 2],
        [40, nodes],
        [44, edges],
        [48, attributes],
        [52, blob],
        [56, blob],
        [60, bytes.length - blob]
    ])
        put(offset, value);
    let cursor = blob;
    const refs = encoded.map((value) => {
        const ref = [cursor, value.length];
        bytes.set(value, cursor);
        cursor += value.length;
        return ref;
    });
    const string = (offset, index) => {
        put(offset, refs[index][0]);
        put(offset + 4, refs[index][1]);
    };
    const node = (index, kind) => {
        const at = nodes + index * 160;
        put(at, kind);
        for (const offset of [8, 12, 16, 20]) put(at + offset, 1);
        for (const offset of [32, 36, 64, 72, 80, 88, 96, 120]) put(at + offset, 0xffff_ffff);
        return at;
    };
    const root = node(0, 1);
    put(root + 120, 1);
    const metadata = node(1, 0x103);
    put(metadata + 4, 0x3f);
    put(metadata + 28, 6);
    for (let index = 0; index < 6; index++) {
        put(edges + index * 4, index + 2);
        const at = node(index + 2, 0x104);
        string(at + 64, index < 2 ? 0 : index - 1);
        put(at + 44, index < 4 ? 1 : 2);
        if (index < 4) put(at + 4, index);
        if (index === 1) view.setBigInt64(at + 56, 1n, true);
        if (index === 2 || index === 3) string(at + 72, index + 3);
        if (index >= 4) {
            put(at + 36, 0);
            put(at + 40, index === 5 ? 2 : 0);
        }
    }
    string(attributes, 7);
    string(attributes + 8, 9);
    string(attributes + 16, 8);
    string(attributes + 24, 10);
    const document = new NodeDecoder(bytes).decodeDocument();
    const bad = bytes.slice();
    new DataView(bad.buffer).setUint32(nodes + 3 * 160 + 4, 9, true);
    assert.throws(() => new NodeDecoder(bad).decodeDocument(), /metadata scalar/);
    bytes.fill(0);
    assert.deepEqual(
        [
            document.metadata.name,
            document.metadata.title,
            document.metadata.subtitle,
            document.metadata.time,
            document.metadata.date,
            document.metadata.authors
        ],
        [
            { kind: "scalar", value: { kind: "null" } },
            { kind: "scalar", value: { kind: "bool", value: true } },
            { kind: "scalar", value: { kind: "number", value: "9007199254740993" } },
            { kind: "scalar", value: { kind: "text", value: "中文\nquoted" } },
            { kind: "list", items: [] },
            {
                kind: "list",
                items: [
                    { kind: "number", value: "1.25" },
                    { kind: "text", value: "" }
                ]
            }
        ]
    );
    assert.equal(document.metadata.comment, null);
    assert.equal(document.metadata.keywords, null);
    assert.match(document.dump(), /subtitle=scalar\(number\("9007199254740993"\)\)/);
    const visited = [];
    walk(
        document,
        walkingVisitor((value, phase) => {
            if (phase === "entering") visited.push(value.kind);
        })
    );
    assert.deepEqual(visited, ["document"]);
});

test("ast: dimensions belong to occurrences and universal attributes survive release", () => {
    const bytes = nativeResult("![a][r] ![b][r]\n\n[r]: /u\n");
    const view = new DataView(bytes.buffer);
    const image = findNode(bytes, kinds.indexOf("media"));
    view.setUint32(image + 124, 640, true);
    view.setUint32(image + 128, 480, true);
    const document = new NodeDecoder(bytes).decodeDocument();
    const images = document.content[0].content.filter((value) => value.kind === "media");
    assert.equal(images[0].dest, images[1].dest);
    assert.deepEqual(
        images.map((value) => value.dimensions),
        [{ width: 640, height: 480 }, null]
    );
    view.setUint32(image + 124, 0, true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /invalid dimensions/);
    view.setUint32(image + 124, 0xffff_ffff, true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /invalid dimensions/);
    view.setUint32(image + 124, 640, true);
    view.setUint32(image + 128, 0xffff_ffff, true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /invalid dimensions/);
    bytes.fill(0);
    const directive = Document.parse(':n{#id .a class="a b}c" k=1 k=2}').content[0].content[0];
    assert.equal(directive.anchor, "id");
    assert.deepEqual(directive.attributes, {
        classes: ["a", "a", "b}c"],
        records: [
            { name: "k", value: "1" },
            { name: "k", value: "2" }
        ]
    });
    assert.ok(directive.dump().includes('attributes={.a .a ."b}c" k="1" k="2"}'));
});

test("ast: cross links retain raw values after native release and reject wrong wire branches", () => {
    const bytes = nativeResult("[[Note]] [[Note|]] ![[#^id|raw *label*]]\n");
    const document = new NodeDecoder(bytes).decodeDocument();
    const links = document.content[0].content.filter(
        (node) => node.kind === "crossLink" || node.kind === "crossEmbedded"
    );
    assert.deepEqual(
        links.map((node) => [node.kind, node.dest, node.label]),
        [
            ["crossLink", { kind: "cross", path: "Note", anchor: null }, null],
            ["crossLink", { kind: "cross", path: "Note", anchor: null }, ""],
            ["crossEmbedded", { kind: "cross", path: "", anchor: "id" }, "raw *label*"]
        ]
    );
    assert.ok(links.every((node) => !("embedded" in node)));
    assert.ok(!("dimensions" in links[0]));
    assert.equal(links[2].dimensions, null);
    const events = [];
    walk(
        links[2],
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, ["entering:crossEmbedded", "exiting:crossEmbedded"]);
    const malformed = bytes.slice();
    new DataView(malformed.buffer).setInt32(findNode(malformed, kinds.indexOf("crossLink")) + 44, 1, true);
    assert.throws(() => new NodeDecoder(malformed).decodeDocument(), /cross reference requires a cross destination/u);
    bytes.fill(0);
    assert.equal(links[2].label, "raw *label*");
});

test("ast: Properties keep recognized fields and literal prose after native release", () => {
    const source =
        "---\r\nname: 9007199254740993\r\nnot YAML\r\n...\r\nunknown: ignored\r\n" +
        "comment: *x\r\nname: duplicate\r\nabstract: |\r\n  first\r\n\r\n  second\r\n" +
        "comment: |\r\n  # prose\r\n---\r\nbody\r\n";
    const bytes = nativeResult(source);
    const document = new NodeDecoder(bytes).decodeDocument();
    bytes.fill(0);
    assert.deepEqual(
        [
            document.metadata.name.value.value,
            document.metadata.abstract.value.value,
            document.metadata.comment.value.value
        ],
        ["9007199254740993", "first\n\nsecond\n", "# prose\n"]
    );
    assert.equal(document.content[0].scope.start.line, 15);
    assert.equal(document.metadata.scope.end.line, 14);
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => {
            if (phase === "entering") events.push(node.kind);
        })
    );
    assert.deepEqual(events, ["document", "paragraph", "text"]);
    const empty = Document.parse("---\nunknown: 1\nfree text\n---").metadata;
    assert.ok(empty);
    assert.ok(Object.entries(empty).every(([key, value]) => key === "scope" || value === null));
    assert.equal(Document.parse("---\nname: 1\n").metadata, null);
});

test("ast: embedded dimensions survive the wire lifetime and require an embedded label", () => {
    const bytes = nativeResult("![[#^id|raw *label*|2147483647x2]]\n");
    const record = findNode(bytes, kinds.indexOf("crossEmbedded"));
    const document = new NodeDecoder(bytes).decodeDocument();
    const link = document.content[0].content[0];
    assert.equal(link.label, "raw *label*");
    assert.deepEqual(link.dimensions, { width: 2147483647, height: 2 });
    const view = new DataView(bytes.buffer);
    view.setUint32(record, kinds.indexOf("crossLink"), true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /dimensions require/u);
    view.setUint32(record, kinds.indexOf("crossEmbedded"), true);
    view.setUint32(record + 124, 0, true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /invalid dimensions/u);
    view.setUint32(record + 124, 100, true);
    view.setUint32(record + 80, 0xffff_ffff, true);
    view.setUint32(record + 84, 0, true);
    assert.throws(() => new NodeDecoder(bytes).decodeDocument(), /dimensions require/u);
    bytes.fill(0);
    assert.equal(link.label, "raw *label*");
    assert.deepEqual(link.dimensions, { width: 2147483647, height: 2 });
    assert.deepEqual(link.dest, { kind: "cross", path: "", anchor: "id" });
});

test("ast: P2 attributes preserve native arrays, inheritance, dimensions and occurrence scopes", () => {
    const document = Document.parse(
        "# T ## {#heading}\n\n`x`{.code} [x][r]{#own .same k=2} ![alt|20x30][r]{width=50% height=2in}\n\n[r]: /u {#definition .same k=1 k=1}\n"
    );
    assert.equal(document.content[0].anchor, "heading");
    const [code, link, image] = document.content[1].content.filter((value) => value.kind !== "text");
    assert.deepEqual(code.attributes.classes, ["code"]);
    assert.equal(code.literal, "x");
    assert.equal(code.scope.end.column, 10);
    assert.equal(link.anchor, "own");
    assert.deepEqual(link.attributes.classes, ["same", "same"]);
    assert.deepEqual(
        link.attributes.records.map((value) => value.value),
        ["1", "1", "2"]
    );
    assert.equal(image.anchor, "definition");
    assert.deepEqual(image.dimensions, { width: 20, height: 30 });
    assert.deepEqual(image.attributes.records.slice(-2), [
        { name: "width", value: "50%" },
        { name: "height", value: "2in" }
    ]);
    assert.equal(link.scope.end.line, 3);
    assert.equal(image.scope.end.line, 3);
    assert.ok(Array.isArray(link.attributes.classes));
    assert.ok(document.dump().includes('anchor="definition"'));
});
