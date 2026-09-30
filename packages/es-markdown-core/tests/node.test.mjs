import { spawnSync } from "node:child_process";
import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, MarkupDumper, markupEquals, walk } from "../dist/index.js";
// Past index.js for the instance itself: the heap is what this asserts about,
// and it is observable without the source carrying anything for the test.
import { native } from "../dist/runtime/native.js";
import { parseDocumentWithNative } from "../dist/runtime/parser.js";
import { Decoder } from "../dist/wire/node-decoder.js";
import { emptyVisitor } from "./visitor.mjs";
import { MessageWriter, nativeMessage } from "./wire.mjs";

test("ast: dimensions belong to each image occurrence while its destination stays shared", () => {
    const source = '![*alt*|2147483647x2][r] ![3][r] ![bad|01][r]\n\n[r]: /shared "title"\n';
    const document = Document.parse(source);
    const images = document.content[0].content.filter((node) => node.kind === "embedded");
    assert.deepEqual(
        images.map((node) => node.dimensions),
        [{ width: 2147483647, height: 2 }, { width: 3, height: null }, null]
    );
    assert.equal(images[0].dest, images[1].dest);
    assert.equal(images[1].dest, images[2].dest);
    assert.equal(images[0].title, "title");
    assert.equal(images[0].content[0].kind, "emphasis");
    assert.equal(images[0].content[0].content[0].literal, "alt");
    assert.equal(document.scope(images[0].content[0], source).end.column, 7);
    assert.deepEqual(images[1].content, []);
    assert.equal(images[2].content[0].literal, "bad|01");
    const events = [];
    walk(
        images[0],
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "enter:Embedded",
        "enter:Emphasis",
        "enter:Text",
        "exit:Text",
        "exit:Emphasis",
        "exit:Embedded"
    ]);
});

test("api: walker controls callback phases and propagates failures", () => {
    const document = Document.parse("text");
    const events = [];
    const observer = walkingVisitor((node, phase) => {
        events.push(`${phase}:${node.kind}`);
    });
    walk(document, observer);
    assert.deepEqual(events, [
        "enter:document",
        "enter:paragraph",
        "enter:text",
        "exit:text",
        "exit:paragraph",
        "exit:document"
    ]);
    const failure = new Error("exit failure");
    const failedEvents = [];
    const failing = walkingVisitor((node, phase) => {
        failedEvents.push(`${phase}:${node.kind}`);
        if (node.kind === "text" && phase === "exit") throw failure;
    });
    assert.throws(
        () => walk(document, failing),
        (error) => error === failure
    );
    assert.deepEqual(failedEvents, ["enter:document", "enter:paragraph", "enter:text", "exit:text"]);
    walk(Document.parse(""), failing);
    assert.deepEqual(failedEvents.slice(4), ["enter:document", "exit:document"]);
});

test("api: typed callbacks collect state and propagate failures", () => {
    const document = Document.parse("# Heading\n\nBody\n");
    const headings = [];
    walk(document, {
        ...emptyVisitor,
        heading(node, phase) {
            if (phase === "enter") headings.push(node.level);
        }
    });
    assert.deepEqual(headings, [1]);
    const failure = new Error("visitor failure");
    assert.throws(
        () =>
            walk(document, {
                ...emptyVisitor,
                document() {
                    throw failure;
                }
            }),
        (error) => error === failure
    );
    assert.throws(() => walk(document, { ...emptyVisitor, document: undefined }), TypeError);
});

test("api: walking dispatch is typed and preserves owned-field semantics", () => {
    const block = Document.parse(":::note[Title]\nBody\n:::\n").content[0];
    const events = [];
    walk(
        block,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );

    assert.deepEqual(events, [
        "enter:DirectiveBlock",
        "enter:DirectiveLabel",
        "enter:Text",
        "exit:Text",
        "exit:DirectiveLabel",
        "enter:Paragraph",
        "enter:Text",
        "exit:Text",
        "exit:Paragraph",
        "exit:DirectiveBlock"
    ]);
    assert.deepEqual(block.content.map(nodeKindName), ["Paragraph"]);

    const source = "| a |\n| --- |\n| b |\n";
    const document = Document.parse(source);
    const table = document.content[0];
    const tableRowKinds = [];
    walk(
        table,
        walkingVisitor((node, phase) => {
            if (phase === "enter" && node.kind === "tableRow")
                tableRowKinds.push(document.scope(node, source).start.line);
        })
    );
    assert.deepEqual(tableRowKinds, [1, 3]);
});

test("ast: marks retain typed content and walk both phases after native release", () => {
    const document = Document.parse("==a *b*==");
    const mark = document.content[0].content[0];
    const counts = [];
    walk(mark, {
        ...emptyVisitor,
        mark: (node, phase) => {
            if (phase === "enter") counts.push(node.content.length);
        }
    });
    assert.deepEqual(counts, [2]);
    const events = [];
    walk(
        mark,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "enter:mark",
        "enter:text",
        "exit:text",
        "enter:emphasis",
        "enter:text",
        "exit:text",
        "exit:emphasis",
        "exit:mark"
    ]);
    assert.equal(mark.content[1].content[0].literal, "b");
    assert.deepEqual(document.scope(mark, "==a *b*=="), { start: { line: 1, column: 1 }, end: { line: 1, column: 9 } });
});

test("ast: insertions retain typed content and walk both phases after native release", () => {
    const document = Document.parse("++a *b*++");
    const insertion = document.content[0].content[0];
    const counts = [];
    walk(insertion, {
        ...emptyVisitor,
        insertion: (node, phase) => {
            if (phase === "enter") counts.push(node.content.length);
        }
    });
    assert.deepEqual(counts, [2]);
    const events = [];
    walk(
        insertion,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "enter:insertion",
        "enter:text",
        "exit:text",
        "enter:emphasis",
        "enter:text",
        "exit:text",
        "exit:emphasis",
        "exit:insertion"
    ]);
    assert.equal(insertion.content[1].content[0].literal, "b");
    assert.deepEqual(document.scope(insertion, "++a *b*++"), {
        start: { line: 1, column: 1 },
        end: { line: 1, column: 9 }
    });
});

test("ast: spans retain typed content and walk both phases after native release", () => {
    const document = Document.parse("[a *b*]{}");
    const span = document.content[0].content[0];
    const counts = [];
    walk(span, {
        ...emptyVisitor,
        span: (node, phase) => {
            if (phase === "enter") counts.push(node.content.length);
        }
    });
    assert.deepEqual(counts, [2]);
    const events = [];
    walk(
        span,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "enter:span",
        "enter:text",
        "exit:text",
        "enter:emphasis",
        "enter:text",
        "exit:text",
        "exit:emphasis",
        "exit:span"
    ]);
    assert.equal(span.content[1].content[0].literal, "b");
    assert.deepEqual(document.scope(span, "[a *b*]{}"), { start: { line: 1, column: 1 }, end: { line: 1, column: 9 } });
});

test("ast: superscripts retain typed content and walk both phases after native release", () => {
    const document = Document.parse("^a*b*^");
    const superscript = document.content[0].content[0];
    const counts = [];
    walk(superscript, {
        ...emptyVisitor,
        superscript: (node, phase) => {
            if (phase === "enter") counts.push(node.content.length);
        }
    });
    assert.deepEqual(counts, [2]);
    const events = [];
    walk(
        superscript,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "enter:superscript",
        "enter:text",
        "exit:text",
        "enter:emphasis",
        "enter:text",
        "exit:text",
        "exit:emphasis",
        "exit:superscript"
    ]);
    assert.equal(superscript.content[1].content[0].literal, "b");
    assert.deepEqual(document.scope(superscript, "^a*b*^"), {
        start: { line: 1, column: 1 },
        end: { line: 1, column: 6 }
    });
});

test("ast: subscripts retain typed content and walk both phases after native release", () => {
    const document = Document.parse("~a*b*~");
    const subscript = document.content[0].content[0];
    const counts = [];
    walk(subscript, {
        ...emptyVisitor,
        subscript: (node, phase) => {
            if (phase === "enter") counts.push(node.content.length);
        }
    });
    assert.deepEqual(counts, [2]);
    const events = [];
    walk(
        subscript,
        walkingVisitor((node, phase) => events.push(`${phase}:${node.kind}`))
    );
    assert.deepEqual(events, [
        "enter:subscript",
        "enter:text",
        "exit:text",
        "enter:emphasis",
        "enter:text",
        "exit:text",
        "exit:emphasis",
        "exit:subscript"
    ]);
    assert.equal(subscript.content[1].content[0].literal, "b");
    assert.deepEqual(document.scope(subscript, "~a*b*~"), {
        start: { line: 1, column: 1 },
        end: { line: 1, column: 6 }
    });
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
        assert.ok(Document.parse(source).dump(source).includes(witness), `${witness} for ${JSON.stringify(source)}`);
    }
});

test("ast: captions and sparse rows survive native release and walk in ownership order", () => {
    const source = ": Caption\n\n+---+---+\n| a | b |\n+   +   +\n| c | d |\n+---+---+\n";
    const document = Document.parse(source);
    const table = document.content[0];
    assert.equal(table.caption.kind, "tableCaption");
    assert.equal(table.caption.content[0].literal, "Caption");
    assert.deepEqual(table.content[1].cells, []);
    assert.equal(table.content[0].cells[0].rowspan, 2);
    assert.equal(table.content[0].cells[0].content[1].content[0].literal, "c");
    const events = [];
    walk(
        table,
        walkingVisitor((node, phase) => {
            if (phase === "enter") events.push(node.kind);
        })
    );
    assert.deepEqual(events.slice(0, 4), ["table", "tableCaption", "text", "tableRow"]);
    assert.deepEqual(document.scope(table.caption, source), {
        start: { line: 1, column: 1 },
        end: { line: 1, column: 9 }
    });
});

test("ast: typed fields are copied from the native result", () => {
    const document = Document.parse("3. item\n\n| a |\n| :-: |\n| b |\n");
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.deepEqual(
        document.content[1].columns.map((column) => column.flow),
        ["center"]
    );
});

test("ast: the document dumps itself and any of its nodes from the source", () => {
    const source = "# Heading\n";
    const document = Document.parse(source);
    assert.equal(document.dump(source), MarkupDumper.dump(document, source));
    assert.match(document.dump(document.content[0], source), /^Heading scope=1:1..1:9 /);
    assert.equal(document.dump(document.content[0], source), MarkupDumper.dump(document, document.content[0], source));
    // Only the contract's fields are enumerable; the unit, the tables and the
    // queries are the document's.
    assert.deepEqual(Object.keys(document), ["kind", "id", "extent", "anchor", "attributes", "content", "metadata"]);
    assert.deepEqual(Object.keys(document.content[0]), [
        "kind",
        "id",
        "extent",
        "anchor",
        "attributes",
        "level",
        "content"
    ]);
    assert.throws(() => document.dump("# Head"), RangeError);
    assert.throws(() => document.dump(Document.parse(source).content[0], source), RangeError);
    assert.throws(() => document.dump(document.content[0]), TypeError);
});

test("unicode: UTF-8 survives native document release", () => {
    const document = Document.parse("héllo 🚀 中文\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
    for (let index = 0; index < 300; index += 1) Document.parse("# copy\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
});

test("errors: empty input is valid and arguments are checked", () => {
    assert.deepEqual(Document.parse("").content, []);
    const empty = Document.parse("");
    assert.deepEqual(empty.scope(empty, ""), { start: { line: 1, column: 1 }, end: { line: 1, column: 0 } });
    const accent = Document.parse("é", { unit: "utf8" });
    assert.deepEqual(accent.scope(accent, "é"), { start: { line: 1, column: 1 }, end: { line: 1, column: 2 } });
    assert.throws(() => Document.parse(null), TypeError);
    assert.throws(() => Document.parse("", { unit: "utf32" }), TypeError);
});

test("errors: allocation failure is terminal across the WASM boundary", () => {
    let parseCalled = false;
    const allocationFailure = {
        memory: new globalThis.WebAssembly.Memory({ initial: 1 }),
        malloc: () => 0,
        free: () => {},
        markdown_core_wire_parse: () => {
            parseCalled = true;
            return 0;
        },
        markdown_core_wire_free: () => {}
    };
    assert.throws(
        () => parseDocumentWithNative(allocationFailure, "text", "utf16"),
        (error) => error?.name === "ParseError" && error.code === "allocationFailed"
    );
    assert.equal(parseCalled, false, "the runtime must not parse or fall back after allocation refusal");

    const memory = new globalThis.WebAssembly.Memory({ initial: 1 });
    const result = new MessageWriter().error(2, "out of memory");
    new Uint8Array(memory.buffer, 64, result.length).set(result);
    const frees = [];
    const freedResults = [];
    const nativeFailure = {
        memory,
        malloc: () => 8,
        free: (pointer) => frees.push(pointer),
        markdown_core_wire_parse: () => 64,
        markdown_core_wire_free: (pointer) => freedResults.push(pointer)
    };
    assert.throws(
        () => parseDocumentWithNative(nativeFailure, "text", "utf16"),
        (error) =>
            error?.name === "ParseError" && error.code === "allocationFailed" && error.message === "out of memory"
    );
    assert.deepEqual(freedResults, [64]);
    assert.deepEqual(frees, [8]);
});

test("ownership: every occurrence of one definition crosses the boundary once and is materialized once", () => {
    // M2: the C tree shares one resource across every occurrence of a
    // definition. The message writes it with the first occurrence and every
    // later one names its ordinal, and the decoder reuses the value it built
    // for the first. Growing the definition therefore grows the message by
    // the definition, never by the definition times its occurrences.
    const count = 20_000;
    const source = (size) => {
        const destination = `/${"u".repeat(size)}`;
        const classes = Array.from({ length: size }, () => ".c").join(" ");
        return `[a]: ${destination} {#${"a".repeat(size)} ${classes} k=${destination}}\n\n${"[a]\n\n".repeat(count)}`;
    };
    const size = 1024;
    const growth = nativeMessage(source(size)).length - nativeMessage(source(1)).length;
    const authored = source(size).length - source(1).length;
    assert.ok(growth < 2 * authored, `the message grew ${growth} bytes for ${authored} authored bytes`);

    const destination = `/${"u".repeat(size)}`;
    const classes = Array.from({ length: size }, () => "c");
    const document = Document.parse(source(size));
    const links = document.content.map((paragraph) => paragraph.content[0]);
    assert.equal(links.length, count);
    assert.equal(links[0].anchor, "a".repeat(size));
    assert.deepEqual(links[0].attributes.classes, classes);
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
});

test("ownership: forward heading references share their finalized target without inheriting heading attributes", () => {
    const count = 5_000;
    const source = (size) => `${"[Target]\n\n".repeat(count)}# Target {#${"a".repeat(size)} .heading k=1}\n`;
    const size = 1024;
    const anchor = "a".repeat(size);
    // The anchor crosses twice, on the heading and in the one shared
    // destination, whatever the number of references.
    const growth = nativeMessage(source(size)).length - nativeMessage(source(1)).length;
    assert.ok(growth < 3 * (size - 1), `the message grew ${growth} bytes`);
    const document = Document.parse(source(size));
    const links = document.content.slice(0, count).map((paragraph) => paragraph.content[0]);
    assert.equal(document.content[count].anchor, anchor);
    assert.deepEqual(links[0].dest, { kind: "url", value: `#${anchor}` });
    assert.ok(links.every((link) => link.dest === links[0].dest));
    assert.ok(links.every((link) => link.anchor === null && link.title === null));
    assert.deepEqual(links[0].attributes, { classes: [], records: [] });
});

test("ast: an ordinary quote is a metadata-free callout", () => {
    const source = "> quote\n";
    const document = Document.parse(source);
    const [callout] = document.content;
    assert.equal(callout.kind, "callout");
    assert.equal(callout.variant, null);
    assert.equal(callout.collapsed, null);
    assert.equal(callout.title, null);
    assert.equal(callout.content.length, 1);
    assert.equal(
        document.dump(source),
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
        "enter:Callout",
        "enter:Strong",
        "enter:Text",
        "exit:Text",
        "exit:Strong",
        "enter:Paragraph",
        "enter:Text",
        "exit:Text",
        "exit:Paragraph",
        "exit:Callout"
    ]);
    const [comment, empty] = Document.parse("> [!note]+ %%t%%\n\n> [!note]\n").content;
    assert.equal(comment.collapsed, false);
    assert.equal(comment.title[0].kind, "comment");
    assert.equal(comment.title[0].literal, "t");
    assert.deepEqual(comment.content, []);
    assert.equal(empty.title, null);
    assert.equal(empty.collapsed, null);
});

test("ast: a title is decoded before the content and dumped as a group", () => {
    // The title is a node-valued field written before the content, whose
    // count is present exactly when a title was authored. This message is
    // written by hand: a document holding one collapsed `note` callout whose
    // title is the text `T` and whose content is empty. The title's first
    // node leads from the callout's start.
    const bytes = new MessageWriter()
        .text("T", { extent: [9, 1] })
        .record("callout", { extent: [0, 8] })
        .optional("note", MessageWriter.prototype.string)
        .optional(true, MessageWriter.prototype.bool)
        .optional(1, MessageWriter.prototype.u32)
        .u32(0)
        .root(1, { extent: [0, 8] })
        .document();

    const document = decoder(bytes).decode();
    const source = "> [!note]-T";
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
        MarkupDumper.dump(document, source),
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
    assert.deepEqual(events, ["enter:Callout", "enter:Text", "exit:Text", "exit:Callout"]);
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
        markdown_core_wire_parse: (...arguments_) => {
            parseCalls += 1;
            return native.markdown_core_wire_parse(...arguments_);
        },
        markdown_core_wire_free: (result) => {
            resultFrees += 1;
            native.markdown_core_wire_free(result);
        }
    };
    assert.equal(parseDocumentWithNative(countedNative, unit.repeat(5_000), "utf16").content.length, 10_000);
    assert.equal(parseCalls, 1, "AST transfer must be independent of node and field count");
    assert.equal(resultFrees, 1, "the one native result must be released exactly once");
    assert.deepEqual(
        Object.keys(native).filter((name) => name.startsWith("markdown_core_node_")),
        [],
        "per-node WASM accessors must not return through the export surface"
    );
});

test("robustness: uncapped list nesting remains traversable", () => {
    // Records arrive in post-order and the decoder builds them on a heap
    // stack, so depth is data rather than native or JS call-stack use.
    const depth = 10_000;
    const document = Document.parse("- ".repeat(depth) + "leaf\n");
    let entered = 0;
    let exited = 0;
    walk(
        document,
        walkingVisitor((_node, phase) => {
            if (phase === "enter") entered += 1;
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

function decoder(bytes, unit = "utf16") {
    return new Decoder(bytes, unit);
}

function walkingVisitor(callback) {
    return Object.fromEntries(Object.keys(emptyVisitor).map((method) => [method, callback]));
}

function nodeKindName(node) {
    return node.kind[0].toUpperCase() + node.kind.slice(1);
}

test("ast: inline notes are owned by their referents and definitions stay where they were written", () => {
    const source = "^[^[x]]\n\n[^inline-1]: authored\n";
    const document = Document.parse(source);
    // The table lists every footnote in source order, inline notes included.
    assert.deepEqual(
        document.footnotes.map((note) => [note.label, note.content[0].kind]),
        [
            [null, "cite"],
            [null, "text"],
            ["inline-1", "paragraph"]
        ]
    );
    const [outer, inner, authored] = document.footnotes;
    const citation = document.content[0].content[0].citations[0];
    assert.deepEqual(citation.referent, { kind: "footnote", target: { kind: "note", footnote: outer } });
    assert.equal(citation.referent.target.footnote, outer);
    assert.equal(outer.content[0].citations[0].referent.target.footnote, inner);
    // The referenced definition is a block in the content that holds it.
    assert.equal(document.content[1], authored);
    // A lookup names the first definition of a label; an inline note has none.
    assert.equal(document.footnote("inline-1"), authored);
    assert.equal(document.footnote("inline-2"), null);
    assert.deepEqual(document.specimens, []);
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "enter:Document",
        "enter:Paragraph",
        "enter:Cite",
        "enter:Citation",
        "enter:Footnote",
        "enter:Cite",
        "enter:Citation",
        "enter:Footnote",
        "enter:Text",
        "exit:Text",
        "exit:Footnote",
        "exit:Citation",
        "exit:Cite",
        "exit:Footnote",
        "exit:Citation",
        "exit:Cite",
        "exit:Paragraph",
        "enter:Footnote",
        "enter:Paragraph",
        "enter:Text",
        "exit:Text",
        "exit:Paragraph",
        "exit:Footnote",
        "exit:Document"
    ]);
    // A note covers `^[...]` while its citation covers only the content, so
    // its lead from the citation's start is negative.
    assert.deepEqual(outer.extent, { lead: -2, span: 7 });
    assert.deepEqual(document.scope(outer, source), { start: { line: 1, column: 1 }, end: { line: 1, column: 7 } });
    assert.deepEqual(document.scope(citation, source), { start: { line: 1, column: 3 }, end: { line: 1, column: 6 } });
});

test("ast: repeated calls name one definition by label, and definitions are content", () => {
    // M4: repeated calls name one footnote by label with empty affixes; the
    // footnote is a block of the content, walked where it was written.
    const source = "[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n";
    const document = Document.parse(source);
    const [paragraph, once, twice] = document.content;
    const cite = paragraph.content[0];
    assert.equal(cite.kind, "cite");
    assert.equal(cite.citations.length, 1);
    assert.deepEqual(cite.citations[0].referent, { kind: "footnote", target: { kind: "label", value: "a" } });
    assert.deepEqual(cite.citations[0].prefix, []);
    assert.deepEqual(cite.citations[0].suffix, []);
    assert.deepEqual(document.scope(cite.citations[0], source), {
        start: { line: 1, column: 2 },
        end: { line: 1, column: 3 }
    });
    assert.deepEqual([once.kind, once.label, twice.kind, twice.label], ["footnote", "a", "footnote", "a"]);
    assert.deepEqual(document.footnotes, [once, twice]);
    assert.equal(document.footnote("a"), once);
    assert.equal(document.footnote("A"), null);
    assert.equal(
        document.dump(source),
        "Document scope=1:1..5:11 anchor=null attributes={} children=3\n" +
            "├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=3\n" +
            "│   ├── Cite scope=1:1..1:4 anchor=null attributes={} children=1\n" +
            '│   │   └── Citation scope=1:2..1:3 anchor=null attributes={} referent=footnote(label="a") children=0\n' +
            "│   │       ├── CitationPrefix children=0\n" +
            "│   │       └── CitationSuffix children=0\n" +
            '│   ├── Text scope=1:5..1:5 anchor=null attributes={} literal=" " children=0\n' +
            "│   └── Cite scope=1:6..1:9 anchor=null attributes={} children=1\n" +
            '│       └── Citation scope=1:7..1:8 anchor=null attributes={} referent=footnote(label="a") children=0\n' +
            "│           ├── CitationPrefix children=0\n" +
            "│           └── CitationSuffix children=0\n" +
            '├── Footnote scope=3:1..4:0 anchor=null attributes={} label="a" children=1\n' +
            "│   └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1\n" +
            '│       └── Text scope=3:7..3:10 anchor=null attributes={} literal="once" children=0\n' +
            '└── Footnote scope=5:1..5:11 anchor=null attributes={} label="a" children=1\n' +
            "    └── Paragraph scope=5:7..5:11 anchor=null attributes={} children=1\n" +
            '        └── Text scope=5:7..5:11 anchor=null attributes={} literal="twice" children=0\n'
    );
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => {
            if (phase === "enter") events.push(nodeKindName(node));
        })
    );
    assert.deepEqual(events, [
        "Document",
        "Paragraph",
        "Cite",
        "Citation",
        "Text",
        "Cite",
        "Citation",
        "Footnote",
        "Paragraph",
        "Text",
        "Footnote",
        "Paragraph",
        "Text"
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

test("errors: malformed native messages are rejected before they enter the AST", () => {
    // The two sides of the wire are built separately, and a decoder that
    // silently mapped an unknown value would turn a protocol mismatch into a
    // wrong document. Each guard is exercised, so none can be removed and
    // stay green.
    const decode = (writer) => decoder(writer.document()).decode();
    const text = () => new MessageWriter().text("t");
    assert.equal(decode(text().root(1)).content[0].literal, "t");

    // Native parse failures keep their terminal category across the WASM
    // boundary. In particular, allocation failure must not be collapsed into
    // an internal error that a consumer could mistake for a recoverable path.
    const failure = (code) => decoder(new MessageWriter().error(code, "bad")).decode();
    assert.throws(
        () => failure(1),
        (error) => error.code === "invalidArgument" && error.message === "bad"
    );
    assert.throws(
        () => failure(2),
        (error) => error.code === "allocationFailed"
    );
    assert.throws(
        () => failure(99),
        (error) => error.code === "internal"
    );

    // Values outside the contract's enums, booleans and branches.
    assert.throws(
        () => decode(new MessageWriter().record("formula").u8(2).string("x").root(1)),
        /invalid enum index 2/u
    );
    assert.throws(
        () => decode(new MessageWriter().record("codeBlock").bool(false).bool(false).string("x").u8(2).root(1)),
        /invalid boolean 2/u
    );
    assert.throws(
        () => decode(new MessageWriter().record("crossLink").u8(2).string("p").bool(false).root(1)),
        /invalid branch 2/u
    );

    // A typed field accepts its own kind only, and content accepts no typed
    // kind: a directive's label is a field, never a generic child.
    assert.throws(
        () => decode(new MessageWriter().record("paragraph").u32(0).record("directive").string("n").bool(true).root(1)),
        /places a paragraph node in a directiveLabel field/u
    );
    assert.throws(
        () => decode(new MessageWriter().record("directiveLabel").u32(0).root(1)),
        /places a directiveLabel node in a content field/u
    );

    // The shape of the message as a whole.
    assert.throws(() => decode(new MessageWriter().record(99)), /unknown node kind 99/u);
    assert.throws(() => decode(new MessageWriter().root(1)), /names more nodes than precede it/u);
    assert.throws(() => decode(text().text("u").root(1)), /not one document tree/u);
    assert.throws(() => decode(text()), /not one document tree/u);
    assert.throws(() => decode(new MessageWriter().record("link").u32(1).u32(0).root(1)), /unknown resource 1/u);
    assert.throws(
        () => decode(new MessageWriter().record("table").bool(false).u32(0xffff_ffff)),
        /count exceeds the message/u
    );

    // An id is exact below 2^53 and invalid at or above it.
    assert.equal(
        decode(new MessageWriter().text("t", { id: 2 ** 53 - 1 }).root(1, { id: 1 })).content[0].id,
        2 ** 53 - 1
    );
    assert.throws(() => decode(new MessageWriter().text("t", { id: 2n ** 53n }).root(1)), /id is not below 2\^53/u);
    assert.throws(
        () => decode(new MessageWriter().text("t", { id: 2n ** 64n - 1n }).root(1)),
        /id is not below 2\^53/u
    );

    // A definition table names nodes of its own kind that the message built.
    const note = () => new MessageWriter().record("footnote", { id: 7 }).bool(false).u32(0);
    assert.equal(decode(note().root(1, { footnotes: [7] })).footnotes[0].id, 7);
    assert.throws(() => decode(note().root(1, { footnotes: [8] })), /footnote table names no footnote node 8/u);
    assert.throws(() => decode(note().root(1, { specimens: [7] })), /specimen table names no specimen node 7/u);
    assert.throws(() => decode(text().root(1, { footnotes: [1] })), /footnote table names no footnote node 1/u);
    const trailing = text().root(1).u8(0).document();
    assert.throws(() => decoder(trailing).decode(), /bytes after its definition tables/u);
    const tableless = text().record("document").u32(1).bool(false).document();
    assert.throws(() => decoder(tableless).decode(), /truncated native result/u);

    const valid = text().root(1).document();
    const truncated = valid.slice(0, -1);
    new DataView(truncated.buffer).setUint32(4, truncated.length, true);
    assert.throws(() => decoder(truncated).decode(), /truncated native result/u);
    assert.throws(() => decoder(Uint8Array.from([...valid, 0])).decode(), /length does not match/u);
    const badMagic = valid.slice();
    badMagic[0] = 0;
    assert.throws(() => decoder(badMagic).decode(), /invalid native result/u);
    const badStatus = valid.slice();
    badStatus[8] = 2;
    assert.throws(() => decoder(badStatus).decode(), /unsupported native result status 2/u);
});

test("ast: every ordered delimiter and associated numbering value survives decoding", () => {
    // Branch indexes of OrderedListVariant and OrderedListDelimiter, in the
    // contract's declaration order.
    const delimiters = [
        [0, null, "period"],
        [1, false, { kind: "parenthesis", closed: false }],
        [1, true, { kind: "parenthesis", closed: true }],
        [2, null, "default"]
    ];
    const list = (variant, lowercased, delimiter, closed) => {
        const writer = new MessageWriter()
            .text("item")
            .record("paragraph")
            .u32(1)
            .record("listItem")
            .bool(false)
            .u32(1)
            .record("list")
            .u8(1)
            .optional(1, MessageWriter.prototype.int)
            .bool(true)
            .u8(variant);
        if (lowercased !== null) writer.bool(lowercased);
        writer.bool(true).u8(delimiter);
        if (closed !== null) writer.bool(closed);
        return decoder(writer.bool(true).u32(1).root(1).document()).decode();
    };
    for (const [variant, kind] of [
        [1, "alpha"],
        [2, "roman"]
    ]) {
        for (const lowercased of [false, true]) {
            for (const [delimiter, closed, expected] of delimiters) {
                const document = list(variant, lowercased, delimiter, closed);
                assert.deepEqual(document.content[0].variant, { kind, lowercased });
                assert.deepEqual(document.content[0].delimiter, expected);
                assert.match(
                    MarkupDumper.dump(document, ""),
                    new RegExp(`variant=${kind}\\(lowercased=${lowercased}\\)`)
                );
                const spelling = typeof expected === "string" ? expected : `parenthesis(closed=${closed})`;
                assert.ok(MarkupDumper.dump(document, "").includes(`delimiter=${spelling}`));
            }
        }
    }
    assert.equal(list(0, null, 0, null).content[0].variant, "decimal");
    assert.equal(list(3, null, 2, null).content[0].variant, "default");
});

test("ast: a UTF-8 task marker is an owned string, independent of the payload", () => {
    const bytes = new MessageWriter()
        .text("x")
        .record("paragraph")
        .u32(1)
        .record("listItem")
        .optional("🚀", MessageWriter.prototype.string)
        .u32(1)
        .record("list")
        .u8(0)
        .bool(false)
        .bool(false)
        .bool(false)
        .bool(true)
        .u32(1)
        .root(1)
        .document();
    const document = decoder(bytes).decode();
    bytes.fill(0);
    assert.equal(document.content[0].items[0].marker, "🚀");
    assert.equal(document.content[0].items[0].completed, true);
    assert.ok(MarkupDumper.dump(document, "").includes('marker="🚀"'));
});

test("ast: specimen definitions and references retain ownership, nulls and reset facts", () => {
    // CitationReferent: bib(0) | footnote(1) | specimen(2); FootnoteTarget:
    // label(0) | note(1).
    const message = (start) => {
        const writer = new MessageWriter();
        writer.record("citation").u8(1).u8(0).string("note").u32(0).u32(0).record("cite").u32(1);
        writer.text(" ");
        writer.record("citation").u8(2).string("étude").u32(0).u32(0).record("cite").u32(1);
        return writer
            .record("paragraph")
            .u32(3)
            .text("note")
            .record("paragraph")
            .u32(1)
            .record("footnote", { id: 20 })
            .optional("note", MessageWriter.prototype.string)
            .u32(1)
            .text("body")
            .record("paragraph")
            .u32(1)
            .record("specimen", { id: 30 })
            .optional("étude", MessageWriter.prototype.string)
            .optional(start, MessageWriter.prototype.int)
            .u32(1)
            .text("tail")
            .record("paragraph")
            .u32(1)
            .record("specimen", { id: 31 })
            .bool(false)
            .bool(false)
            .u32(1)
            .root(4, { footnotes: [20], specimens: [30, 31] })
            .document();
    };
    assert.throws(() => decoder(message(9007199254740993n)).decode(), /precision/);
    const bytes = message(5);
    const document = decoder(bytes).decode();
    bytes.fill(0);
    assert.deepEqual(
        document.content.map((node) => node.kind),
        ["paragraph", "footnote", "specimen", "specimen"]
    );
    assert.deepEqual(document.footnotes, [document.content[1]]);
    assert.deepEqual(document.specimens, [document.content[2], document.content[3]]);
    assert.deepEqual(
        document.specimens.map(({ label, start }) => [label, start]),
        [
            ["étude", 5],
            [null, null]
        ]
    );
    assert.equal(document.specimen("étude"), document.content[2]);
    assert.equal(document.specimen("etude"), null);
    assert.equal(document.footnote("note"), document.content[1]);
    assert.equal(document.specimens[0].content[0].content[0].literal, "body");
    assert.deepEqual(document.content[0].content[0].citations[0].referent, {
        kind: "footnote",
        target: { kind: "label", value: "note" }
    });
    assert.deepEqual(document.content[0].content[2].citations[0].referent, { kind: "specimen", label: "étude" });
    const dumped = document.dump("");
    assert.match(dumped, /Specimen scope=\S+ anchor=null attributes=\{\} label="étude" start=5 children=1/u);
    assert.match(dumped, /Specimen scope=\S+ anchor=null attributes=\{\} label=null start=null children=1/u);
    assert.match(dumped, /referent=footnote\(label="note"\)[^]*referent=specimen\(label="étude"\)/u);
    const events = [];
    walk(
        document,
        walkingVisitor((value, phase) => {
            if (phase === "enter") events.push(value.kind);
        })
    );
    assert.equal(events.filter((kind) => kind === "specimen").length, 2);
    assert.ok(events.indexOf("footnote") < events.indexOf("specimen"));
});

test("ast: table groups, column widths and spans survive the wire as owned facts", () => {
    const writer = new MessageWriter();
    const row = (...literals) => {
        for (const literal of literals)
            writer
                .text(literal)
                .record("tableCell")
                .int(1)
                .int(literal === "f" ? 2 : 1)
                .u32(1);
        writer.record("tableRow").u32(literals.length);
    };
    row("h", "i");
    row("b", "c");
    row("f", "g");
    // No caption; two columns; one row in each of head, content and foot.
    writer.record("table").bool(false).u32(2).u8(0).optional(0.1, MessageWriter.prototype.double);
    const bytes = writer.u8(3).bool(false).u32(1).u32(1).u32(1).root(1).document();
    const document = decoder(bytes).decode();
    bytes.fill(0);
    const value = document.content[0];
    assert.equal(value.head[0].cells[0].content[0].literal, "h");
    assert.equal(value.content[0].cells[0].content[0].literal, "b");
    assert.equal(value.foot[0].cells[0].content[0].literal, "f");
    assert.equal(value.foot[0].cells[0].colspan, 2);
    assert.deepEqual(value.columns, [
        { flow: "none", relative: 0.1 },
        { flow: "right", relative: null }
    ]);
    assert.ok(!("isHeader" in value.head[0]));
    const visited = [];
    walk(
        value,
        walkingVisitor((node, phase) => {
            if (phase === "enter" && node.kind === "text") visited.push(node.literal);
        })
    );
    assert.deepEqual(visited, ["h", "i", "b", "c", "f", "g"]);
    assert.match(document.dump(value, ""), /columns=\[none:0.1,right:null\] children=3/);
    assert.match(document.dump(value, ""), /TableFoot children=1/);
});

test("ast: metadata preserves tags, decimal text, duplicate keys and owned lists", () => {
    // MetadataValue: scalar(0) | list(1); MetadataScalar: null(0) | bool(1) |
    // number(2) | text(3); MetadataListItem: number(0) | text(1).
    const message = (nameScalar) =>
        new MessageWriter()
            .record("metadata")
            .bool(true)
            .u8(0)
            .u8(nameScalar)
            .bool(true)
            .u8(0)
            .u8(1)
            .bool(true)
            .bool(true)
            .u8(0)
            .u8(2)
            .string("9007199254740993")
            .bool(true)
            .u8(0)
            .u8(3)
            .string("中文\nquoted")
            .bool(true)
            .u8(1)
            .u32(0)
            .bool(true)
            .u8(1)
            .u32(2)
            .u8(0)
            .string("1.25")
            .u8(1)
            .string("")
            .bool(false)
            .bool(false)
            .bool(false)
            .bool(false)
            .root(0, { metadata: true })
            .document();
    assert.throws(() => decoder(message(9)).decode(), /invalid branch 9/);
    const bytes = message(0);
    const document = decoder(bytes).decode();
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
    assert.match(document.dump(""), /subtitle=scalar\(number\("9007199254740993"\)\)/);
    const visited = [];
    walk(
        document,
        walkingVisitor((value, phase) => {
            if (phase === "enter") visited.push(value.kind);
        })
    );
    assert.deepEqual(visited, ["document", "metadata"]);
});

test("ast: dimensions belong to occurrences and universal attributes survive release", () => {
    const bytes = nativeMessage("![a|640x480][r] ![b][r]\n\n[r]: /u\n");
    const document = decoder(bytes).decode();
    bytes.fill(0);
    const images = document.content[0].content.filter((value) => value.kind === "embedded");
    assert.equal(images[0].dest, images[1].dest);
    assert.deepEqual(
        images.map((value) => value.dimensions),
        [{ width: 640, height: 480 }, null]
    );
    const source = ':n{#id .a class="a b}c" k=1 k=2}';
    const parsed = Document.parse(source);
    const directive = parsed.content[0].content[0];
    assert.equal(directive.anchor, "id");
    assert.deepEqual(directive.attributes, {
        classes: ["a", "a", "b}c"],
        records: [
            { name: "k", value: "1" },
            { name: "k", value: "2" }
        ]
    });
    assert.ok(parsed.dump(directive, source).includes('attributes={.a .a ."b}c" k="1" k="2"}'));
});

test("ast: cross links retain raw values after native release", () => {
    const bytes = nativeMessage("[[Note]] [[Note|]] ![[#^id|raw *label*]]\n");
    const document = decoder(bytes).decode();
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
    assert.deepEqual(events, ["enter:crossEmbedded", "exit:crossEmbedded"]);
    bytes.fill(0);
    assert.equal(links[2].label, "raw *label*");
});

test("ast: Properties keep recognized fields and literal prose after native release", () => {
    const source =
        "---\r\nname: 9007199254740993\r\nnot YAML\r\n...\r\nunknown: ignored\r\n" +
        "comment: *x\r\nname: duplicate\r\nabstract: |\r\n  first\r\n\r\n  second\r\n" +
        "comment: |\r\n  # prose\r\n---\r\nbody\r\n";
    const bytes = nativeMessage(source);
    const document = decoder(bytes).decode();
    bytes.fill(0);
    assert.deepEqual(
        [
            document.metadata.name.value.value,
            document.metadata.abstract.value.value,
            document.metadata.comment.value.value
        ],
        ["9007199254740993", "first\n\nsecond\n", "# prose\n"]
    );
    assert.equal(document.scope(document.content[0], source).start.line, 15);
    assert.equal(document.scope(document.metadata, source).end.line, 14);
    const events = [];
    walk(
        document,
        walkingVisitor((node, phase) => {
            if (phase === "enter") events.push(node.kind);
        })
    );
    assert.deepEqual(events, ["document", "metadata", "paragraph", "text"]);
    const empty = Document.parse("---\nunknown: 1\nfree text\n---").metadata;
    assert.ok(empty);
    assert.ok(
        Object.entries(empty).every(
            ([key, value]) => ["kind", "id", "extent", "anchor", "attributes"].includes(key) || value === null
        )
    );
    assert.equal(Document.parse("---\nname: 1\n").metadata, null);
});

test("ast: embedded dimensions survive the wire lifetime", () => {
    const bytes = nativeMessage("![[#^id|raw *label*|2147483647x2]]\n");
    const document = decoder(bytes).decode();
    bytes.fill(0);
    const link = document.content[0].content[0];
    assert.equal(link.label, "raw *label*");
    assert.deepEqual(link.dimensions, { width: 2147483647, height: 2 });
    assert.deepEqual(link.dest, { kind: "cross", path: "", anchor: "id" });
});

test("ast: P2 attributes preserve native arrays, inheritance, dimensions and occurrence scopes", () => {
    const source =
        "# T ## {#heading}\n\n`x`{.code} [x][r]{#own .same k=2} ![alt|20x30][r]{width=50% height=2in}\n\n[r]: /u {#definition .same k=1 k=1}\n";
    const document = Document.parse(source);
    assert.equal(document.content[0].anchor, "heading");
    const [code, link, image] = document.content[1].content.filter((value) => value.kind !== "text");
    assert.deepEqual(code.attributes.classes, ["code"]);
    assert.equal(code.literal, "x");
    assert.equal(document.scope(code, source).end.column, 10);
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
    assert.equal(document.scope(link, source).end.line, 3);
    assert.equal(document.scope(image, source).end.line, 3);
    assert.ok(Array.isArray(link.attributes.classes));
    assert.ok(document.dump(source).includes('anchor="definition"'));
});

test("ast: definition terms and ordered bodies are owned and walk without body wrapper nodes", () => {
    const bytes = nativeMessage("::: box\n*T*\n: one\n~\n\nU\n\n: two\n:::\n");
    const block = decoder(bytes).decode().content[0];
    bytes.fill(0);
    assert.equal(block.name, null);
    assert.deepEqual(block.attributes.classes, ["box"]);
    const list = block.content[0];
    assert.equal(list.kind, "definitionList");
    assert.equal(list.kind, "definitionList");
    const first = list.definitions[0];
    assert.equal(first.kind, "definition");
    assert.equal(first.compact, true);
    assert.equal(first.term[0].content[0].literal, "T");
    assert.equal(first.content[0][0].content[0].literal, "one");
    assert.deepEqual(first.content[1], []);
    assert.equal(list.definitions[1].compact, false);
    const events = [];
    walk(
        list,
        walkingVisitor((node, phase) => events.push(`${phase}:${nodeKindName(node)}`))
    );
    assert.deepEqual(events, [
        "enter:DefinitionList",
        "enter:Definition",
        "enter:Emphasis",
        "enter:Text",
        "exit:Text",
        "exit:Emphasis",
        "enter:Paragraph",
        "enter:Text",
        "exit:Text",
        "exit:Paragraph",
        "exit:Definition",
        "enter:Definition",
        "enter:Text",
        "exit:Text",
        "enter:Paragraph",
        "enter:Text",
        "exit:Text",
        "exit:Paragraph",
        "exit:Definition",
        "exit:DefinitionList"
    ]);
});

test("errors: definition bodies hold content and definition lists hold definitions only", () => {
    const decode = (writer) => decoder(writer.root(1).document()).decode();
    // Term `T`; two bodies, the first empty and the second holding `b`.
    const definition = decode(
        new MessageWriter()
            .text("T")
            .text("b")
            .record("definition")
            .u32(1)
            .u32(2)
            .u32(0)
            .u32(1)
            .bool(true)
            .record("definitionList")
            .u32(1)
    ).content[0].definitions[0];
    assert.deepEqual(
        definition.content.map((body) => body.map((node) => node.literal)),
        [[], ["b"]]
    );
    assert.throws(
        () => decode(new MessageWriter().record("paragraph").u32(0).record("definitionList").u32(1)),
        /places a paragraph node in a definition field/u
    );
    assert.throws(
        () =>
            decode(
                new MessageWriter()
                    .text("T")
                    .record("listItem")
                    .bool(false)
                    .u32(0)
                    .record("definition")
                    .u32(1)
                    .u32(1)
                    .u32(1)
                    .bool(false)
                    .record("definitionList")
                    .u32(1)
            ),
        /places a listItem node in a content field/u
    );
});

test("api: owned elements are Markup with finite walks and preserved labels", () => {
    const source = "---\ntitle: Example\n---\n[^Label]\n\n[^label]: self [^LABEL]\n\n(@sample) Body\n";
    const document = Document.parse(source);
    const citation = document.content[0].content[0].citations[0];
    const nodes = [document.metadata, citation, document.footnotes[0], document.specimens[0]];
    assert.deepEqual(
        nodes.map((node) => node.kind),
        ["metadata", "citation", "footnote", "specimen"]
    );
    for (const node of nodes) {
        assert.equal(node.anchor, null);
        assert.deepEqual(node.attributes, { classes: [], records: [] });
        assert.equal(document.dump(node, source), MarkupDumper.dump(document, node, source));
        const events = [];
        walk(
            node,
            walkingVisitor((value, phase) => events.push(`${phase}:${value.kind}`))
        );
        assert.equal(events[0], `enter:${node.kind}`);
        assert.equal(events.at(-1), `exit:${node.kind}`);
        assert.equal(events.filter((event) => event.startsWith("enter:")).length, events.length / 2);
    }
    assert.deepEqual(citation.referent, { kind: "footnote", target: { kind: "label", value: "label" } });
    assert.equal(document.footnotes[0].label, "label");
    assert.equal(document.footnote("label"), document.footnotes[0]);
    assert.equal(document.specimen("sample"), document.specimens[0]);
    // Owned kinds live only in the fields that name them, never in content.
    for (const kind of ["metadata", "citation"]) {
        const writer = new MessageWriter().record(kind);
        if (kind === "metadata") for (let field = 0; field < 10; field += 1) writer.bool(false);
        if (kind === "citation") writer.u8(1).u8(0).string("label").u32(0).u32(0);
        assert.throws(
            () => decoder(writer.record("paragraph").u32(1).root(1).document()).decode(),
            new RegExp(`places a ${kind} node in a content field`, "u")
        );
    }
    // An inline note's Footnote is the first node its citation takes, and
    // it must be a Footnote.
    const noted = (kind) => {
        const writer = new MessageWriter().text("x").record(kind).bool(false);
        if (kind === "specimen") writer.bool(false);
        return decoder(
            writer
                .u32(1)
                .record("citation")
                .u8(1)
                .u8(1)
                .u32(0)
                .u32(0)
                .record("cite")
                .u32(1)
                .record("paragraph")
                .u32(1)
                .root(1)
                .document()
        ).decode();
    };
    const inline = noted("footnote").content[0].content[0].citations[0];
    assert.equal(inline.referent.target.footnote.content[0].literal, "x");
    assert.throws(() => noted("specimen"), /places a specimen node in a footnote field/u);
});

test("api: scope queries count columns in the document's unit from the extents and the source", () => {
    const scope = (startLine, startColumn, endLine, endColumn) => ({
        start: { line: startLine, column: startColumn },
        end: { line: endLine, column: endColumn }
    });
    const scopes = (source, unit) => {
        const document = Document.parse(source, { unit });
        assert.equal(document.unit, unit);
        const found = [];
        walk(
            document,
            walkingVisitor((node, phase) => {
                if (phase === "enter") found.push([node.kind, document.scope(node, source)]);
            })
        );
        return found;
    };
    // A zero-byte document, and one holding only a line terminator, end at
    // the column before the first.
    for (const unit of ["utf8", "utf16"]) {
        assert.deepEqual(scopes("", unit), [["document", scope(1, 1, 1, 0)]]);
        assert.deepEqual(scopes("\n", unit), [["document", scope(1, 1, 1, 0)]]);
    }
    // `é` is two UTF-8 bytes and one UTF-16 unit; `🚀` is four bytes and two
    // units.
    assert.deepEqual(scopes("é🚀x\n", "utf8"), [
        ["document", scope(1, 1, 1, 7)],
        ["paragraph", scope(1, 1, 1, 7)],
        ["text", scope(1, 1, 1, 7)]
    ]);
    assert.deepEqual(scopes("é🚀x\n", "utf16"), [
        ["document", scope(1, 1, 1, 4)],
        ["paragraph", scope(1, 1, 1, 4)],
        ["text", scope(1, 1, 1, 4)]
    ]);
    // CRLF is one terminator; a lone CR ends a line too.
    const crlf = "a\r\nb 🚀c\r\n\r\n- é\r";
    assert.deepEqual(scopes(crlf, "utf8"), [
        ["document", scope(1, 1, 4, 4)],
        ["paragraph", scope(1, 1, 2, 7)],
        ["text", scope(1, 1, 1, 1)],
        ["softBreak", scope(1, 2, 1, 2)],
        ["text", scope(2, 1, 2, 7)],
        ["list", scope(4, 1, 4, 4)],
        ["listItem", scope(4, 1, 4, 4)],
        ["paragraph", scope(4, 3, 4, 4)],
        ["text", scope(4, 3, 4, 4)]
    ]);
    assert.deepEqual(scopes(crlf, "utf16"), [
        ["document", scope(1, 1, 4, 3)],
        ["paragraph", scope(1, 1, 2, 5)],
        ["text", scope(1, 1, 1, 1)],
        ["softBreak", scope(1, 2, 1, 2)],
        ["text", scope(2, 1, 2, 5)],
        ["list", scope(4, 1, 4, 3)],
        ["listItem", scope(4, 1, 4, 3)],
        ["paragraph", scope(4, 3, 4, 3)],
        ["text", scope(4, 3, 4, 3)]
    ]);
    // A node of another document, or a source shorter than the document's,
    // has no scope.
    const document = Document.parse("é🚀x\n");
    assert.equal(document.scope(Document.parse("é🚀x\n").content[0], "é🚀x\n"), null);
    assert.equal(document.scope(document.content[0], "é🚀"), null);
    // The default unit is UTF-16.
    assert.equal(document.unit, "utf16");
});

test("api: nodeAt finds the last node in walk order holding the byte at a position", () => {
    const at = (document, source, line, column) => document.nodeAt({ line, column }, source);
    const source = "é🚀x\n";
    const wide = Document.parse(source);
    const text = wide.content[0].content[0];
    assert.equal(at(wide, source, 1, 1), text);
    assert.equal(at(wide, source, 1, 2), text);
    // Between the halves of a surrogate pair: not a scalar boundary.
    assert.equal(at(wide, source, 1, 3), null);
    assert.equal(at(wide, source, 1, 4), text);
    // The terminator is a byte of line 1 that no node holds; past it the
    // position names no byte of the line.
    assert.equal(at(wide, source, 1, 5), null);
    assert.equal(at(wide, source, 1, 6), null);
    assert.equal(at(wide, source, 2, 1), null);
    assert.equal(at(wide, source, 0, 1), null);
    assert.equal(at(wide, source, 1, 0), null);
    assert.equal(at(wide, source, 1.5, 1), null);
    const narrow = Document.parse(source, { unit: "utf8" });
    const bytes = narrow.content[0].content[0];
    assert.equal(at(narrow, source, 1, 1), bytes);
    assert.equal(at(narrow, source, 1, 2), null);
    assert.equal(at(narrow, source, 1, 3), bytes);
    assert.equal(at(narrow, source, 1, 4), null);
    assert.equal(at(narrow, source, 1, 7), bytes);
    const crlf = "a\r\nb 🚀c\r\n\r\n- é\r";
    const document = Document.parse(crlf);
    const [paragraph, list] = document.content;
    assert.equal(at(document, crlf, 1, 2), paragraph.content[1]);
    assert.equal(at(document, crlf, 1, 3), paragraph);
    assert.equal(at(document, crlf, 2, 5), paragraph.content[2]);
    assert.equal(at(document, crlf, 4, 3), list.items[0].content[0].content[0]);
    assert.equal(at(document, crlf, 4, 4), null);
    // A nested inline note is the last node holding its bytes.
    const noted = "a ^[b] c\n";
    const notes = Document.parse(noted);
    assert.equal(at(notes, noted, 1, 3), notes.footnotes[0]);
    assert.equal(at(notes, noted, 1, 5), notes.footnotes[0].content[0]);
});

test("api: markupEquals is deep value equality including ids, keyed by id", () => {
    const source = "# Title\n\n> - a *b* [^n]\n\n[^n]: note ^[inline]\n\n| x |\n| - |\n| y |\n";
    const left = Document.parse(source);
    const right = Document.parse(source);
    assert.notEqual(left, right);
    assert.ok(markupEquals(left, right));
    assert.ok(markupEquals(left, left));
    assert.ok(markupEquals(left.content[1], right.content[1]));
    assert.equal(markupEquals(left.content[0], right.content[1]), false);
    // A difference at the deepest leaf, in an extent alone, or in the unit's
    // absence from the contract.
    assert.equal(markupEquals(left, Document.parse(source.replace("*b*", "*c*"))), false);
    assert.equal(markupEquals(Document.parse("# a\n"), Document.parse("#  a\n")), false);
    assert.ok(markupEquals(left, Document.parse(source, { unit: "utf8" })));
    // Ids alone: the same tree with other ids is another value.
    const message = (id) =>
        new MessageWriter().text("t", { id }).record("paragraph", { id: 2 }).u32(1).root(1, { id: 1 });
    assert.ok(markupEquals(decoder(message(3).document()).decode(), decoder(message(3).document()).decode()));
    assert.equal(markupEquals(decoder(message(3).document()).decode(), decoder(message(4).document()).decode()), false);
    // The id is the key: equal nodes have equal ids, so a key finds the
    // equal node in the other document.
    const byId = new Map();
    walk(
        right,
        walkingVisitor((node, phase) => {
            if (phase === "enter") byId.set(node.id, node);
        })
    );
    walk(
        left,
        walkingVisitor((node, phase) => {
            if (phase === "enter") assert.ok(markupEquals(node, byId.get(node.id)));
        })
    );
});

test("robustness: deep dumps run with a bounded JavaScript call stack", () => {
    const module = new URL("../dist/index.js", import.meta.url).href;
    const script = `
        import assert from "node:assert/strict";
        import { Document } from ${JSON.stringify(module)};
        const depth = 512;
        const source = "- ".repeat(depth) + "leaf\\n";
        const lines = Document.parse(source).dump(source).trimEnd().split("\\n");
        assert.equal(lines.length, depth * 2 + 3);
        assert.ok(lines[0].startsWith("Document "));
        assert.ok(lines.at(-1).includes('literal="leaf"'));
        assert.ok(lines.at(-1).startsWith("    ".repeat(depth * 2 + 1) + "└── "));
    `;
    const result = spawnSync(process.execPath, ["--stack_size=128", "--input-type=module", "-e", script], {
        encoding: "utf8"
    });
    assert.equal(result.status, 0, result.stderr);
});

test("robustness: deep trees are released, compared, walked and queried with a small fixed stack", () => {
    // Nothing follows tree edges by recursion: a regression overflows this
    // stack deterministically at these depths.
    const module = new URL("../dist/index.js", import.meta.url).href;
    const script = `
        import assert from "node:assert/strict";
        import { Document, markupEquals, walk } from ${JSON.stringify(module)};
        for (const depth of [30_000, 65_536]) {
            const source = "- ".repeat(depth) + "leaf\\n";
            const deepest = (document) => {
                let list = document.content[0];
                for (let level = 1; level < depth; level += 1) list = list.items[0].content[0];
                return list;
            };
            // A view keeps the deepest list while its document is released.
            let document = Document.parse(source);
            const released = new WeakRef(document);
            const view = deepest(document);
            document = null;
            await new Promise((resolve) => setTimeout(resolve, 0));
            globalThis.gc();
            assert.equal(released.deref(), undefined);
            assert.equal(view.items[0].content[0].content[0].literal, "leaf");

            const left = Document.parse(source);
            assert.ok(markupEquals(left, Document.parse(source)));
            assert.equal(markupEquals(left, Document.parse("- ".repeat(depth) + "lead\\n")), false);

            let entered = 0;
            let exited = 0;
            const count = (_node, phase) => {
                if (phase === "enter") entered += 1;
                else exited += 1;
            };
            walk(left, new Proxy({}, { get: () => count }));
            assert.equal(entered, depth * 2 + 3);
            assert.equal(exited, entered);

            const leaf = deepest(left).items[0].content[0].content[0];
            const column = depth * 2 + 1;
            const scope = { start: { line: 1, column }, end: { line: 1, column: column + 3 } };
            assert.deepEqual(left.scope(leaf, source), scope);
            assert.equal(left.nodeAt({ line: 1, column }, source), leaf);
            assert.equal(
                left.dump(leaf, source),
                \`Text scope=1:\${column}..1:\${column + 3} anchor=null attributes={} literal="leaf" children=0\\n\`
            );
        }
    `;
    const result = spawnSync(
        process.execPath,
        ["--stack_size=128", "--expose-gc", "--input-type=module", "-e", script],
        {
            encoding: "utf8"
        }
    );
    assert.equal(result.status, 0, result.stderr);
});
