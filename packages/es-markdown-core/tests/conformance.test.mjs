import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";
import { Document, TreeDumper, visit } from "../dist/index.js";
import { kindVisitor } from "./visitor.mjs";

const canonicalFixtures = new URL("../build/generated/conformance/canonical-ast-fixtures.json", import.meta.url);
const canonicalManifest = JSON.parse(await readFile(canonicalFixtures, "utf8"));
if (canonicalManifest.schemaVersion !== 1 || !canonicalManifest.cases?.length) {
    throw new Error("shared canonical AST manifest v1 must contain at least one case");
}

test("conformance: public node schema is reachable", () => {
    const sources = [
        '# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n\n[ref]: /r "t"\n\n[a][ref] ![b][ref]\n',
        'Text *em* **strong** ~~strike~~ ==mark== `code` [link](/go "title") ![alt](/image.png) :badge[label]{kind=demo} $x$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n',
        "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
        "$$\ny\n$$\n",
        "a <!-- b --> c\n\n<!-- block -->\n"
    ];
    const documents = sources.map((source) => Document.parse(source));
    const kinds = documents.flatMap((document) => dumpKinds(document.dump()));
    assert.deepEqual(
        new Set(kinds),
        new Set([
            "document",
            "callout",
            "paragraph",
            "heading",
            "thematicBreak",
            "list",
            "listItem",
            "codeBlock",
            "htmlBlock",
            "formulaBlock",
            "table",
            "tableRow",
            "tableCell",
            "directiveBlock",
            "directiveLabel",
            "text",
            "softBreak",
            "lineBreak",
            "code",
            "html",
            "comment",
            "formula",
            "emphasis",
            "strong",
            "strikethrough",
            "mark",
            "link",
            "media",
            "directive",
            "cite"
        ])
    );
    assert.ok(documents.every((document) => document.scope.start.line === 1 && document.scope.start.column === 1));
});

test("conformance: fields, nullability, and typed table nodes map to JavaScript", () => {
    const document = Document.parse(
        '3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image "title")\n'
    );
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.equal(document.content[0].variant, "decimal");
    assert.equal(document.content[0].delimiter, "period");
    assert.deepEqual(Document.parse("1) item\n").content[0].delimiter, {
        kind: "parenthesis",
        closed: false
    });
    assert.equal(document.content[0].tight, true);
    assert.equal(document.content[1].items[0].marker, "x");
    assert.equal(document.content[1].items[0].tasked, true);
    assert.equal(document.content[1].items[0].completed, true);
    assert.deepEqual(
        document.content[2].columns.map((column) => column.alignment),
        ["center"]
    );
    assert.equal(document.content[2].head.length, 1);
    assert.equal(document.content[2].content.length, 1);
    assert.deepEqual(document.content[2].foot, []);
    assert.equal(document.content[2].head[0].cells.length, 1);
    assert.equal(document.content[2].head[0].cells[0].content[0].literal, "a");
    assert.equal(document.content[2].content[0].cells[0].content[0].literal, "b");
    assert.equal(
        visit(document.content[2].head[0], {
            ...kindVisitor,
            visitTableRow: () => "row"
        }),
        "row"
    );
    assert.equal(
        visit(document.content[2].head[0].cells[0], {
            ...kindVisitor,
            visitTableCell: () => "cell"
        }),
        "cell"
    );
    const link = document.content[3].content[0];
    const image = document.content[3].content[2];
    assert.deepEqual(link.dest, { kind: "url", value: "/go" });
    assert.equal(link.title, null);
    assert.deepEqual(image.dest, { kind: "url", value: "/image" });
    assert.equal(image.title, "title");
});

test("conformance: directive labels preserve missing, empty, and populated states", () => {
    const document = Document.parse(":missing{id=1}\n\n:empty[]\n\n:label[text]\n\n::block[title]\n");
    const missing = document.content[0].content[0];
    const empty = document.content[1].content[0];
    const label = document.content[2].content[0];
    const block = document.content[3];

    assert.equal(missing.label, null);
    assert.equal(missing.anchor, "1");
    assert.deepEqual(missing.attributes, { classes: [], records: [] });
    // A label written empty is Markup in the label field, not directive
    // content. Its scope still distinguishes it from a label never written.
    assert.equal(empty.label.kind, "directiveLabel");
    assert.deepEqual(empty.label.content, []);
    assert.equal(label.label.content[0].literal, "text");
    assert.equal(block.label.content[0].literal, "title");
    assert.deepEqual(block.content, []);

    assert.equal("content" in label, false);
    assert.deepEqual(
        label.label.content.map((node) => node.kind),
        ["text"]
    );
    assert.match(TreeDumper.dump(label), /DirectiveLabel/u);
});

for (const testCase of canonicalManifest.cases) {
    test(`conformance: shared canonical AST case ${testCase.name}`, async () => {
        const document = Document.parse(testCase.source);
        assert.equal(TreeDumper.dump(document), testCase.expected, testCase.name);
        assert.equal(document.dump(), testCase.expected, testCase.name);
    });
}

function dumpKinds(dump) {
    return (
        dump
            .trimEnd()
            .split("\n")
            // A group line has no scope and a value line names a scoped value,
            // and neither is a kind (M4).
            .filter((line) => / scope=/.test(line))
            .map((line) => line.replace(/^[│ ├└─]*/u, "").split(" ", 1)[0])
            .filter((name) => name !== "Citation" && name !== "Footnote")
            .map((name) => (name.startsWith("HTML") ? `html${name.slice(4)}` : name[0].toLowerCase() + name.slice(1)))
    );
}

test("conformance: task markers preserve scalars and derive completion", () => {
    for (const marker of [" ", "x", "X", "?", "é", "✓", "🚀", "́", "]"]) {
        const item = Document.parse(`- [${marker}] body\n`).content[0].items[0];
        assert.equal(item.marker, marker);
        assert.equal(item.tasked, true);
        assert.equal(item.completed, marker !== " ");
    }
    const item = Document.parse("- [é] body\n").content[0].items[0];
    assert.equal(item.marker, null);
    assert.equal(item.tasked, false);
    assert.equal(item.completed, false);
});
