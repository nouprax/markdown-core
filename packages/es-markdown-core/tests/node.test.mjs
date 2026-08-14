import assert from "node:assert/strict";
import { test } from "node:test";
import { DiagnosticCode, Document, MarkupDumper, visit, MarkupWalker, WalkEvent } from "../dist/index.js";
import { unprunedDecode } from "../dist/document.js";
import { kindVisitor } from "./visitor.mjs";

test("api: synchronous parse, typed visitor dispatch, and walker", () => {
    const document = Document("# Heading\n\nBody\n");
    assert.equal(
        visit(document.content[0], {
            ...kindVisitor,
            visitHeading: (node) => `heading:${node.level}`
        }),
        "heading:1"
    );
    const events = [];
    new MarkupWalker().walk(document, (event, node) => events.push(`${event}-${node.kind}`));
    assert.equal(events[0], `${WalkEvent.entering}-document`);
    assert.equal(events.at(-1), `${WalkEvent.exiting}-document`);
});

test("api: the walker's typed-visitor overload dispatches once per node in preorder", () => {
    // The scope-free overload: no events, no scopes, one exhaustive visitor
    // call per node in preorder. Table rows and cells are first-class here,
    // which is what a consumer relying on exhaustive dispatch depends on.
    const document = Document("| a |\n| --- |\n| b |\n");
    const visited = [];
    new MarkupWalker().walk(document, {
        ...kindVisitor,
        visitDocument: (node) => visited.push(node.kind),
        visitTable: (node) => visited.push(node.kind),
        visitTableRow: (node) => visited.push(node.kind),
        visitTableCell: (node) => visited.push(node.kind),
        visitText: (node) => visited.push(node.kind)
    });
    assert.deepEqual(visited, ["document", "table", "tableRow", "tableCell", "text", "tableRow", "tableCell", "text"]);
});

test("api: options gate extensions", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    assert.equal(Document(markdown).content[0].kind, "table");
    assert.equal(Document(markdown, { tables: false }).content[0].kind, "paragraph");

    const directive = "Use :note[text].\n";
    assert.equal(Document(directive).dump().includes("Directive"), true);
    assert.equal(Document(directive, { directives: false }).dump().includes("Directive"), false);

    const source = "before [[folder/note#^block|display]] and ![[folder/note#^block|display]] after\n";
    const paragraph = Document(source).content[0];
    assert.equal(paragraph.content[1].kind, "crossLink");
    assert.equal(paragraph.content[1].reference, "folder/note#^block|display");
    assert.equal(paragraph.content[3].kind, "embed");
    assert.equal(paragraph.content[3].reference, "folder/note#^block|display");
    const linksDisabled = Document(source, { crossLinks: false }).content[0];
    assert.equal(linksDisabled.content[1].kind, "embed");
    const embedsDisabled = Document(source, { embeds: false }).content[0];
    assert.equal(embedsDisabled.content[1].kind, "crossLink");
});

test("api: formulas gates every formula syntax", () => {
    const markdown = [
        "Inline $d$ and \\\\(l\\\\).",
        "",
        "$$",
        "display-dollar",
        "$$",
        "",
        "\\\\[",
        "display-latex",
        "\\\\]",
        "",
        "```formula",
        "fenced",
        "```",
        ""
    ].join("\n");
    const enabled = Document(markdown);
    assert.deepEqual(
        enabled.content.map((node) => node.kind),
        ["paragraph", "formulaBlock", "formulaBlock", "formulaBlock"]
    );
    assert.deepEqual(
        enabled.content[0].content.filter((node) => node.kind === "formula").map((node) => node.literal),
        ["d", "l"]
    );

    const disabled = Document(markdown, { formulas: false });
    assert.equal(disabled.dump().includes("Formula"), false);
    assert.equal(disabled.content.at(-1).kind, "codeBlock");
});

test("ast: typed fields are copied from direct WASM accessors", () => {
    const document = Document("3. item\n\n| a |\n| :-: |\n| b |\n");
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.deepEqual(document.content[1].alignments, ["center"]);
});

test("ast: the document mediates the canonical diagnostic dump", () => {
    const document = Document("Lead\n\n# Heading\n");
    assert.equal(document.dump(), MarkupDumper.dump(document));
    // A subtree dump prints scopes with the subtree as origin.
    assert.match(MarkupDumper.dump(document, document.content[1]), /^Heading scope=1:1\.\.1:9 level=1/);
    // The mediators are non-enumerable; the data — scope included — is not.
    assert.equal(Object.keys(document).includes("dump"), false);
    assert.equal(Object.keys(document).includes("scope"), true);
});

test("ast: nodes carry identity and their own extent", () => {
    const document = Document("Lead\n\n# Heading\n");
    assert.equal(typeof document.id.series, "string");
    assert.match(document.id.series, /^[0-9a-f]{16}$/);
    assert.equal(typeof document.id.rawValue, "number");
    assert.equal(typeof document.revision, "number");
    // The extent is a property OF the node, read without a lookup and
    // without the document that produced it.
    assert.deepEqual(document.content[1].scope, {
        start: { line: 3, column: 1 },
        end: { line: 3, column: 9 }
    });
    // Separate parses never share identity.
    assert.notEqual(Document("# Heading\n").id.series, document.id.series);
});

test("ast: a document survives JSON and its ids come back usable", () => {
    const document = Document("# Title\n\nBody with *emphasis* and `code`.\n");
    // The whole point of rendering the salt as text: no custom serializer,
    // and a round trip that is the document rather than a picture of it.
    const revived = JSON.parse(JSON.stringify(document));
    assert.deepEqual(revived, JSON.parse(JSON.stringify(document)));
    assert.deepEqual(revived.content[1].scope, document.content[1].scope);
    // An id read back out of JSON is the same identity, so the live document
    // still answers for it — as a new object, which is why it is resolved
    // rather than compared by reference.
    const heading = revived.content[0];
    assert.notEqual(heading.id, document.content[0].id);
    assert.deepEqual(heading.id, document.content[0].id);
    assert.equal(document.node(heading.id), document.content[0]);
    // And an id from another series is still not this document's to answer.
    const other = Document("# Other\n");
    assert.equal(document.node(JSON.parse(JSON.stringify(other)).content[0].id), null);
    other.close();
    document.close();
});

test("ast: diagnostics travel with the document that raised them", () => {
    // The one thing an editor underlines: a directive's `{...}` did not
    // parse, so the braces stayed literal text. Invisible in the tree — the
    // node simply has no attributes — which is why it is reported.
    const flagged = Document(":::note{= bad}\nbody\n:::\n");
    assert.deepEqual(
        flagged.diagnostics.map((diagnostic) => diagnostic.code),
        [DiagnosticCode.directiveAttributes]
    );
    assert.deepEqual(flagged.diagnostics[0].scope, {
        start: { line: 1, column: 8 },
        end: { line: 1, column: 14 }
    });

    // A document chain grows one way: append. Replacing the text is a new
    // document (new chain, new series) — and the fixed text reports nothing.
    const fixed = Document(":::note{a=1}\nbody\n:::\n");
    assert.equal(fixed.diagnostics.length, 0);
    // The flagged document keeps reporting its own: diagnostics are values
    // it copied out at parse time, like every other part of it.
    assert.equal(flagged.diagnostics.length, 1);
    fixed.close();
    flagged.close();
});

test("unicode: UTF-8 survives native document release", () => {
    const document = Document("héllo 🚀 中文\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
    for (let index = 0; index < 300; index += 1) Document("# copy\n");
    assert.equal(document.content[0].content[0].literal, "héllo 🚀 中文");
});

test("unicode: a surrogate pair split across appends reassembles intact", () => {
    // Each append's chunk crosses the boundary as UTF-8 on its own, so a
    // non-BMP character torn between two chunks would become U+FFFD twice.
    // The binding holds an unpaired trailing high surrogate back one append
    // — the parsed text trails by at most that one code unit — and the pair
    // reaches the encoder whole when its low half arrives.
    const source = "pre 😀🚀 post\n";
    const whole = Document(source);
    let document = Document("");
    for (let index = 0; index < source.length; index += 1) {
        // Per UTF-16 code unit — unlike `for...of`, this tears every pair.
        const previous = document;
        document = document.append(source[index]);
        previous.close();
        // The value mirror holds on every tick, held unit or not: the
        // pruned decode deep-equals a mirror-free decode of the same parse.
        assert.deepStrictEqual(document, unprunedDecode(document));
    }
    assert.equal(document.content[0].content[0].literal, "pre 😀🚀 post");
    assert.equal(document.dump(), whole.dump());
    whole.close();
    document.close();
});

test("unicode: a lone low surrogate still encodes to U+FFFD", () => {
    // Nothing held and nothing to wait for: no later chunk can complete a
    // low surrogate, so it is the caller's garbage, and it crosses the
    // boundary exactly as TextEncoder always sent it — as U+FFFD.
    const opened = Document("");
    const next = opened.append("a\uDE00");
    assert.equal(next.content[0].content[0].literal, "a�");
    opened.close();
    next.close();
});

test("errors: empty input is valid and arguments are checked", () => {
    assert.deepEqual(Document("").content, []);
    assert.throws(() => Document(null), TypeError);
    assert.throws(() => Document("x", { tables: "yes" }), TypeError);
    assert.throws(() => Document("x", { tables: null }), TypeError);
    assert.throws(() => new Document(), TypeError);
    // append checks its own argument before the surrogate hold-back can
    // coerce a non-string into a string chunk.
    const document = Document("");
    assert.throws(() => document.append(1), TypeError);
    document.close();
});

test("ownership: declarations are readonly without runtime freeze", () => {
    const document = Document("text\n");
    assert.equal(Object.isFrozen(document), false);
    assert.equal(Object.isFrozen(document.content), false);
});

test("robustness: large documents copy completely before native release", () => {
    const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    assert.equal(Document(unit.repeat(5_000)).content.length, 10_000);
});

test("robustness: deep block quote nesting remains traversable", () => {
    const depth = 128;
    let node = Document("> ".repeat(depth) + "leaf\n").content[0];
    for (let index = 0; index < depth; index += 1) {
        assert.equal(node.kind, "blockQuote");
        node = node.content[0];
    }
    assert.equal(node.kind, "paragraph");
});

test("robustness: repeated parse and release remains stable", () => {
    for (let index = 0; index < 2_000; index += 1) {
        assert.equal(Document("# Copy\n\n- [x] item 🚀\n").content.length, 2);
    }
});

test("robustness: worker threads own isolated engine instances", async () => {
    // The engine holds no process-global state and the module instantiates
    // one WASM instance per JS context: workers parsing with disagreeing
    // option sets must reproduce the main thread's dumps byte-for-byte.
    const { Worker } = await import("node:worker_threads");
    const sources = [
        "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
        "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
        "Formula $x^2$ inline and *a$b*c$ flanking.\n\n$$\nx = y\n$$\n",
        ':::note[Label]{id=1 title="T"}\ncontent *here*\n:::\n\nInline :dir[text]{k=v} tail.\n'
    ];
    const variants = [
        undefined,
        {
            smartPunctuation: false,
            footnotes: false,
            tables: false,
            strikethrough: false,
            autolinks: false,
            taskLists: false,
            formulas: false,
            directives: false,
            crossLinks: false,
            embeds: false
        },
        {
            strikethrough: false,
            formulas: false
        }
    ];
    const jobs = sources.flatMap((source) => variants.map((options) => ({ source, options })));
    const references = jobs.map(({ source, options }) => Document(source, options).dump());

    const workers = Array.from(
        { length: 4 },
        () =>
            new Promise((resolve, reject) => {
                const worker = new Worker(new URL("./worker-parse.mjs", import.meta.url), {
                    workerData: { jobs }
                });
                worker.once("message", resolve);
                worker.once("error", reject);
            })
    );
    for (const dumps of await Promise.all(workers)) {
        assert.deepEqual(dumps, references);
    }

    // The same corpus reached by line-by-line APPENDS instead of one-shot
    // parses, still one WASM instance per thread: incremental state is per
    // instance, so four threads appending at once must land on the same
    // trees a one-shot parse produces.
    const appenders = Array.from(
        { length: 4 },
        () =>
            new Promise((resolve, reject) => {
                const worker = new Worker(new URL("./worker-append.mjs", import.meta.url), {
                    workerData: { jobs }
                });
                worker.once("message", resolve);
                worker.once("error", reject);
            })
    );
    for (const dumps of await Promise.all(appenders)) {
        assert.deepEqual(dumps, references);
    }
});

test("robustness: worker threads never mint the same document series", async () => {
    const { Worker } = await import("node:worker_threads");
    // Each worker is a fresh WASM instance whose allocator state and coarse
    // clocks repeat exactly across threads, so only host entropy can keep
    // their salts apart. If it cannot, two unrelated documents' identities
    // compare equal — raw values restart at 1 for every series.
    const series = await Promise.all(
        Array.from(
            { length: 8 },
            () =>
                new Promise((resolve, reject) => {
                    const worker = new Worker(new URL("./worker-series.mjs", import.meta.url));
                    worker.once("message", resolve);
                    worker.once("error", reject);
                })
        )
    );
    assert.equal(series.length, 8);
    assert.ok(series.every((value) => value !== "0"));
    assert.equal(new Set(series).size, 8);
});
