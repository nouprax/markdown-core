import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";
import { Document, MarkupDumper, visit, MarkupWalker, WalkEvent } from "../dist/index.js";
import { kindVisitor } from "./visitor.mjs";

const canonicalFixtures = new URL("../build/generated/conformance/canonical-ast-fixtures.json", import.meta.url);
const canonicalManifest = JSON.parse(await readFile(canonicalFixtures, "utf8"));
if (canonicalManifest.schemaVersion !== 1 || !canonicalManifest.cases?.length) {
    throw new Error("shared canonical AST manifest v1 must contain at least one case");
}

test("conformance: public node schema is reachable", () => {
    const sources = [
        "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n",
        'Text *em* **strong** ~~strike~~ `code` [link](/go "title") ![alt](/image.png) :badge[label]{kind=demo} $x$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n',
        "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
        "$$\ny\n$$\n"
    ];
    const documents = sources.map((source) => Document(source));
    const nodes = documents.flatMap(flatten);
    assert.deepEqual(
        new Set(nodes.map((node) => node.kind)),
        new Set([
            "document",
            "blockQuote",
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
            "footnoteDefinition",
            "text",
            "softBreak",
            "lineBreak",
            "code",
            "html",
            "formula",
            "emphasis",
            "strong",
            "strikethrough",
            "link",
            "image",
            "directive",
            "footnoteReference"
        ])
    );
    assert.ok(documents.every((document) => document.scope.start.line === 1 && document.scope.start.column === 1));
});

test("conformance: fields, nullability, and typed table nodes map to JavaScript", () => {
    const document = Document('3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image "title")\n');
    assert.equal(document.content[0].flavor, "ordered");
    assert.equal(document.content[0].start, 3);
    assert.equal(document.content[0].tight, true);
    assert.equal(document.content[1].items[0].checked, true);
    assert.deepEqual(document.content[2].alignments, ["center"]);
    assert.equal(document.content[2].header.isHeader, true);
    assert.equal(document.content[2].rows[0].isHeader, false);
    assert.equal(document.content[2].header.cells.length, 1);
    assert.equal(document.content[2].header.cells[0].content[0].literal, "a");
    assert.equal(document.content[2].rows[0].cells[0].content[0].literal, "b");
    assert.equal(
        visit(document.content[2].header, {
            ...kindVisitor,
            visitTableRow: (node) => (node.isHeader ? "header" : "row")
        }),
        "header"
    );
    assert.equal(
        visit(document.content[2].header.cells[0], {
            ...kindVisitor,
            visitTableCell: () => "cell"
        }),
        "cell"
    );
    const link = document.content[3].content[0];
    const image = document.content[3].content[2];
    assert.equal(link.destination, "/go");
    assert.equal(link.title, null);
    assert.equal(image.source, "/image");
    assert.equal(image.title, "title");
});

test("conformance: directive labels preserve missing, empty, and populated states", () => {
    const document = Document(":missing{id=1}\n\n:empty[]\n\n:label[text]\n\n::block[title]\n");
    const missing = document.content[0].content[0];
    const empty = document.content[1].content[0];
    const label = document.content[2].content[0];
    const block = document.content[3];

    assert.equal(missing.label, null);
    assert.deepEqual(missing.attributes, { id: "1" });
    assert.equal(empty.label.kind, "directiveLabel");
    assert.deepEqual(empty.label.content, []);
    assert.equal(label.label.kind, "directiveLabel");
    assert.equal(label.label.content[0].literal, "text");
    assert.equal(
        visit(label.label, {
            ...kindVisitor,
            visitDirectiveLabel: (node) => `label:${node.content.length}`
        }),
        "label:1"
    );
    assert.equal(block.label.kind, "directiveLabel");
    assert.equal(block.label.content[0].literal, "title");
    assert.deepEqual(block.content, []);
});

for (const testCase of canonicalManifest.cases) {
    test(`conformance: shared canonical AST case ${testCase.name}`, async () => {
        const document = Document(testCase.source, testCase.parseOptions);
        assert.equal(MarkupDumper.dump(document), testCase.expected, testCase.name);
        assert.equal(document.dump(), testCase.expected, testCase.name);
    });
}

// Edit replay of the shared canonical AST corpus: every per-line revision
// must dump byte-equal to a one-shot parse of the same text, and the delta
// must be exactly the difference between consecutive mirrors.
for (const testCase of canonicalManifest.cases) {
    test(`conformance: edit replay of canonical AST case ${testCase.name}`, () => {
        let document = Document("", testCase.parseOptions);
        let replayed = "";
        let previous = new Map();
        for (const chunk of lineChunks(testCase.source)) {
            replayed += chunk;
            const editing = document;
            const commit = document.edit(replayed);
            document = commit.document;
            editing.close();

            // Equivalence: the edited document dumps byte-equal to a
            // one-shot parse of the same text.
            const reference = Document(replayed, testCase.parseOptions);
            assert.equal(document.dump(), reference.dump(), testCase.name);
            reference.close();

            // Delta integrity: no id is named twice, every node the delta
            // does not name kept its exact revision, and every retired id is
            // gone from the tree.
            const delta = commit.delta;
            const named = delta.diffs.map((diff) => diff.markup.rawValue);
            assert.equal(new Set(named).size, named.length, testCase.name);
            const namedSet = new Set(named);
            const current = new Map();
            for (const node of flatten(document)) {
                current.set(node.id.rawValue, node.revision);
                if (!namedSet.has(node.id.rawValue)) {
                    assert.equal(node.revision, previous.get(node.id.rawValue), testCase.name);
                }
            }
            for (const diff of delta.diffs) {
                if (diff.parts.retired) assert.equal(current.has(diff.markup.rawValue), false, testCase.name);
            }
            previous = current;
        }
        assert.equal(document.dump(), testCase.expected, testCase.name);
        document.close();
    });
}

function lineChunks(source) {
    return source.length === 0 ? [] : source.split(/(?<=\n)/);
}

function flatten(root) {
    const nodes = [];
    new MarkupWalker().walk(root, (event, node) => {
        if (event === WalkEvent.entering) nodes.push(node);
    });
    return nodes;
}
