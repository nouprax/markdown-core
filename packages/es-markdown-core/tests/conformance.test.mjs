import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";
import { Document, MarkupDumper, visit, MarkupWalker, WalkEvent } from "../dist/index.js";
import { unprunedDecode } from "../dist/document.js";
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
// must dump byte-equal to a one-shot parse of the same text, and the
// (id, revision) update protocol must hold against a cumulative ledger —
// the same double walk the C harness runs (tests/support/edit_replay.c).
for (const testCase of canonicalManifest.cases) {
    test(`conformance: edit replay of canonical AST case ${testCase.name}`, () => {
        let document = Document("", testCase.parseOptions);
        let replayed = "";
        // Cumulative across the whole replay: rawValue -> {revision, alive}.
        // Retired entries stay forever, which is what makes a resurrection
        // detectable at any later step, not just the very next one.
        const ledger = new Map();
        // The empty parse seeds the ledger: every node it has is a mint.
        let previousNodes = verifyTree(document, ledger, new Map(), testCase.name);
        for (const chunk of lineChunks(testCase.source)) {
            replayed += chunk;
            const editing = document;
            document = document.edit(replayed);
            editing.close();

            // Equivalence: the edited document dumps byte-equal to a
            // one-shot parse of the same text.
            const reference = Document(replayed, testCase.parseOptions);
            assert.equal(document.dump(), reference.dump(), testCase.name);
            reference.close();

            previousNodes = verifyTree(document, ledger, previousNodes, testCase.name);
        }
        assert.equal(document.dump(), testCase.expected, testCase.name);
        document.close();
    });
}

// The same double walk driven by the real append: each per-line revision now
// arrives as a trailing mutation whose decode is pruned through the value
// mirror, so on top of the ledger every step is compared field by field
// against a mirror-free decode of the same native document — the one oracle
// a wrongly carried value cannot hide from.
for (const testCase of canonicalManifest.cases) {
    test(`conformance: append replay of canonical AST case ${testCase.name}`, () => {
        let document = Document("", testCase.parseOptions);
        let replayed = "";
        const ledger = new Map();
        let previousNodes = verifyTree(document, ledger, new Map(), testCase.name);
        for (const chunk of lineChunks(testCase.source)) {
            replayed += chunk;
            const appending = document;
            document = document.append(chunk);
            appending.close();

            const reference = Document(replayed, testCase.parseOptions);
            assert.equal(document.dump(), reference.dump(), testCase.name);
            reference.close();
            assert.deepStrictEqual(document, unprunedDecode(document), testCase.name);

            previousNodes = verifyTree(document, ledger, previousNodes, testCase.name);
        }
        assert.equal(document.dump(), testCase.expected, testCase.name);
        document.close();
    });
}

/**
 * One visit carries the whole per-node contract, mirroring the C harness's
 * er_walk_visit: no id appears twice in one tree, a retired id never comes
 * back, a child's revision never exceeds its parent's, a minted id carries
 * the new document revision, a survivor's revision is its last sighting or
 * the new document revision, and revisions never regress. The subtree form
 * of the (id, revision) promise — equal pair means an equal subtree —
 * follows from these node-local checks by induction: an unchanged parent
 * pins its child id list, the child-below-parent revision bound forces each
 * child's revision to be an old value, the two-value rule then forces it to
 * be the child's own last sighting, and the child's own visit compares its
 * projection.
 *
 * Returns this tree's rawValue -> node map, the next walk's predecessor.
 */
function verifyTree(document, ledger, previousNodes, name) {
    // Any content change stamps the root, so whenever there is a fresh
    // revision for a node to carry, the root's revision is it.
    const successorRevision = document.revision;
    const seen = new Set();
    const currentNodes = new Map();
    const parents = [];
    new MarkupWalker().walk(document, (event, node) => {
        if (event === WalkEvent.exiting) {
            parents.pop();
            return;
        }
        const parent = parents[parents.length - 1];
        parents.push(node);
        const raw = node.id.rawValue;
        assert.ok(raw > 0, `${name}: a tree node has no id`);
        if (parent !== undefined) {
            assert.ok(node.revision <= parent.revision, `${name}: a child's revision exceeds its parent's`);
        }
        assert.equal(seen.has(raw), false, `${name}: one id appears twice in one tree`);
        seen.add(raw);
        currentNodes.set(raw, node);
        const entry = ledger.get(raw);
        if (entry === undefined) {
            assert.equal(node.revision, successorRevision, `${name}: a minted node lacks the new document revision`);
            ledger.set(raw, { revision: node.revision, alive: true });
            return;
        }
        assert.ok(entry.alive, `${name}: a retired id came back`);
        assert.ok(node.revision >= entry.revision, `${name}: a node's revision went backwards`);
        if (node.revision !== entry.revision) {
            assert.equal(node.revision, successorRevision, `${name}: a changed node lacks the new document revision`);
        } else {
            const before = previousNodes.get(raw);
            assert.notEqual(before, undefined, `${name}: a live ledger id is missing from the predecessor tree`);
            assert.deepEqual(projection(node), projection(before), `${name}: a node changed without a revision bump`);
        }
        entry.revision = node.revision;
    });
    // Retire the absentees; the ledger remembers them forever.
    for (const [raw, entry] of ledger) {
        if (entry.alive && !seen.has(raw)) entry.alive = false;
    }
    return currentNodes;
}

/**
 * A node's own projection: every enumerable field except its extent, with
 * nested nodes reduced to their ids. Scope is position, not content — a pure
 * shift moves it without a revision bump — and child nodes are compared at
 * their own visit, so here they count as their identity alone.
 */
function projection(node) {
    return Object.fromEntries(
        Object.entries(node)
            .filter(([key]) => key !== "scope")
            .map(([key, value]) => [key, dereferenced(value)])
    );
}

function dereferenced(value) {
    if (Array.isArray(value)) return value.map(dereferenced);
    if (isMarkup(value)) return value.id.rawValue;
    return value;
}

function isMarkup(value) {
    return typeof value === "object" && value !== null && "kind" in value && "id" in value && "revision" in value;
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
