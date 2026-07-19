import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, MarkupDumper, visit, MarkupWalker, WalkEvent } from "../dist/index.js";
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
    new MarkupWalker().walk(document, (event, node) => events.push(`${event}-${node.kind}`));
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

test("ast: the document mediates the canonical diagnostic dump", () => {
    const document = Document.parse("Lead\n\n# Heading\n");
    assert.equal(document.dump(), MarkupDumper.dump(document));
    // A subtree dump prints scopes with the subtree as origin.
    assert.match(MarkupDumper.dump(document, document.content[1]), /^Heading scope=1:1\.\.1:9 level=1/);
    assert.equal(Object.keys(document).includes("dump"), false);
    assert.equal(Object.keys(document).includes("scope"), false);
});

test("ast: nodes carry identity instead of positions", () => {
    const document = Document.parse("# Heading\n");
    assert.equal(typeof document.id.lineage, "bigint");
    assert.equal(typeof document.id.rawValue, "number");
    assert.equal(typeof document.revision, "number");
    assert.equal("scope" in document.content[0], false);
    assert.equal(document.scope(document.content[0]).start.line, 1);
    // Separate parses never share identity.
    assert.notEqual(Document.parse("# Heading\n").id.lineage, document.id.lineage);
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
    assert.throws(() => new Document(), TypeError);
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
            stripHTMLComments: false,
            tables: false,
            strikethrough: false,
            autolinks: false,
            taskLists: false,
            formulas: false,
            dollarFormulaDelimiters: false,
            latexFormulaDelimiters: false,
            directives: false
        },
        {
            strikethrough: false,
            formulas: false,
            dollarFormulaDelimiters: false,
            latexFormulaDelimiters: false
        }
    ];
    const jobs = sources.flatMap((source) => variants.map((options) => ({ source, options })));
    const references = jobs.map(({ source, options }) => Document.parse(source, options).dump());

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
});
