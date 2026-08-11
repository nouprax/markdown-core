import assert from "node:assert/strict";
import test from "node:test";
import { DiagnosticCode, Document, MarkupWalker, WalkEvent } from "../dist/index.js";

/**
 * Asserts the delta's own ordering contract: every surviving node appears
 * after all of its own children, so a consumer materializing immutable
 * values bottom-up reads the list once, front to back.
 */
function assertPostorder(delta, document) {
    const position = new Map();
    delta.diffs.forEach((diff, index) => {
        if (!diff.parts.retired) position.set(diff.markup.rawValue, index);
    });
    const stack = [];
    new MarkupWalker().walk(document, (event, node) => {
        if (event === WalkEvent.entering) {
            const parent = stack[stack.length - 1];
            const above = parent === undefined ? undefined : position.get(parent);
            const below = position.get(node.id.rawValue);
            if (above !== undefined && below !== undefined) {
                assert.ok(below < above, `child ${node.id.rawValue} follows its parent`);
            }
            stack.push(node.id.rawValue);
        } else {
            stack.pop();
        }
    });
}

function flatten(document) {
    const nodes = [];
    new MarkupWalker().walk(document, (event, node) => {
        if (event === WalkEvent.entering) nodes.push(node);
    });
    return nodes;
}

test("edits: extending the trailing text keeps its identity and bumps its revision", () => {
    const first = Document("# Title\n\nHello");
    const firstHeading = first.content[0];
    const firstParagraph = first.content[1];
    const firstText = firstParagraph.content[0];

    const commit = first.edit("# Title\n\nHello world");
    const second = commit.document;
    const secondHeading = second.content[0];
    const secondParagraph = second.content[1];
    const secondText = secondParagraph.content[0];

    assert.equal(secondText.literal, "Hello world");
    assert.equal(secondParagraph.id, firstParagraph.id);
    assert.equal(secondText.id, firstText.id);
    assert.ok(secondText.revision > firstText.revision);
    // The heading settled before the edit: not named at all, and the very
    // same object, because an untouched node is never re-decoded.
    assert.equal(secondHeading, firstHeading);

    const named = new Set(commit.delta.diffs.map((diff) => diff.markup));
    assert.equal(named.has(firstHeading.id), false);
    assert.equal(named.has(secondText.id), true);
    assert.ok(commit.delta.diffs.every((diff) => !diff.parts.retired));
    assert.ok(commit.delta.diffs.find((diff) => diff.markup === secondText.id).parts.text);
    second.close();
});

test("edits: a clean-boundary insert at the top leaves downstream identity intact", () => {
    const before = Document("First\n\nSecond\n\nThird\n");
    const downstreamBefore = before.content.map((node) => [node.id, node.revision]);
    const thirdBefore = before.content[2];

    const commit = before.edit("# New\n\nFirst\n\nSecond\n\nThird\n");
    const after = commit.document;

    assert.equal(after.content.length, 4);
    const inserted = after.content[0];
    after.content.slice(1).forEach((node, index) => {
        assert.equal(node.id, downstreamBefore[index][0]);
        assert.equal(node.revision, downstreamBefore[index][1]);
    });

    // A created node differs from absence in every part it has: a heading
    // has a value and children, and no text of its own.
    const insertedDiff = commit.delta.diffs.find((diff) => diff.markup === inserted.id);
    assert.deepEqual(insertedDiff.parts, {
        retired: false,
        value: true,
        text: false,
        children: true,
        descendant: true
    });

    // Downstream nodes shifted by two lines: equal values, new scopes. A
    // value carried over from the predecessor resolves against the successor
    // at its NEW position — identity survives the edit, position does not —
    // and the predecessor still answers at its own.
    assert.equal(after.scope(after.content[3]).start.line, 7);
    assert.equal(after.scope(thirdBefore).start.line, 7);
    assert.equal(before.scope(thirdBefore).start.line, 5);
    const reference = Document("# New\n\nFirst\n\nSecond\n\nThird\n");
    assert.equal(after.dump(), reference.dump());
    reference.close();
    after.close();
});

test("edits: a kind change retires the old identity and mints a new one", () => {
    const before = Document("text\n");
    const paragraph = before.content[0];

    const commit = before.edit("# text\n");
    const after = commit.document;
    const heading = after.content[0];
    assert.notEqual(heading.id.rawValue, paragraph.id.rawValue);

    const rows = commit.delta.diffs;
    assert.ok(rows.some((diff) => diff.markup.rawValue === paragraph.id.rawValue && diff.parts.retired));
    assert.ok(rows.some((diff) => diff.markup === heading.id && !diff.parts.retired));
    // A retired node is emitted WHERE IT WAS FOUND — inside its former
    // parent's run, before what replaced it there and before that parent's
    // own row.
    const at = (rawValue) => rows.findIndex((diff) => diff.markup.rawValue === rawValue);
    assert.ok(at(paragraph.id.rawValue) < at(heading.id.rawValue));
    assert.ok(at(heading.id.rawValue) < at(after.id.rawValue));
    assert.equal(after.node(paragraph.id), null);
    after.close();
});

test("edits: equality is lineage-salted identity plus revision", () => {
    const source = "Same *content* twice.\n";
    const first = Document(source);
    const second = Document(source);
    // Identical content from different parses never shares identity.
    assert.notEqual(first.id.lineage, second.id.lineage);
    assert.notEqual(first.content[0], second.content[0]);
    // An id from another lineage is not this document's to answer.
    assert.equal(first.node(second.content[0].id), null);
    assert.equal(first.node(first.content[0].id), first.content[0]);
    // The root answers for itself, which is what a delta naming it needs.
    assert.equal(first.node(first.id), first);
    first.close();
    second.close();
});

test("edits: a blank-line-only edit reports an empty delta yet shifts scopes", () => {
    const before = Document("Alpha\n\n\n\nOmega\n");
    const omegaBefore = before.content[1];
    assert.equal(before.scope(omegaBefore).start.line, 5);

    // Delete two of the blank lines: no node's content changes.
    const commit = before.edit("Alpha\n\nOmega\n");
    const after = commit.document;
    assert.equal(commit.delta.diffs.length, 0);
    assert.ok(commit.delta.afterRevision > commit.delta.beforeRevision);
    // Nothing was named, so every top-level block is the same object.
    assert.equal(after.content[1], omegaBefore);
    assert.equal(after.scope(after.content[1]).start.line, 3);
    const reference = Document("Alpha\n\nOmega\n");
    assert.equal(after.dump(), reference.dump());
    reference.close();
    after.close();
});

test("edits: a deep rebuild names children before parents in one postorder pass", () => {
    const depth = 512;
    const stable = "Stable\n\n";
    const prefix = "> ".repeat(depth);
    const before = Document(stable + prefix + "alpha\n");
    const stableBefore = before.content[0];

    const commit = before.edit(stable + prefix + "bravo\n");
    const after = commit.document;

    assert.ok(commit.delta.diffs.length >= depth);
    assertPostorder(commit.delta, after);
    // The settled paragraph is untouched, so it is the same object.
    assert.equal(after.content[0], stableBefore);

    // Exactly one node's own projection differs — the innermost text — and
    // every one of its ancestors carries `descendant` and nothing else,
    // which is what lets a renderer stop at the first node whose own parts
    // are all false.
    let innermost = after.content[1];
    while (innermost.kind === "blockQuote") innermost = innermost.content[0];
    const text = innermost.content[0];
    assert.equal(text.literal, "bravo");
    const ownChanges = commit.delta.diffs.filter(
        (diff) => diff.parts.value || diff.parts.text || diff.parts.children || diff.parts.retired
    );
    assert.deepEqual(
        ownChanges.map((diff) => diff.markup),
        [text.id]
    );
    // The chain, the paragraph it ends in, and the document above it.
    assert.equal(commit.delta.diffs.length, depth + 3);
    after.close();
});

test("edits: a superseded document keeps answering from its own tables", () => {
    const first = Document("One\n\nTwo\n");
    const two = first.content[1];
    assert.equal(first.scope(two).start.line, 3);

    // Editing hands the native parse to the successor, which is then
    // released. The predecessor's values, scopes, diagnostics, and dump were
    // all extracted at parse time and owe that parse nothing.
    first.edit("Zero\n\nOne\n\nTwo\n").document.close();
    assert.equal(first.scope(two).start.line, 3);
    assert.ok(first.dump().includes("Paragraph"));
    assert.equal(flatten(first).length, 5);
});

test("edits: an edited or closed document refuses a second edit", () => {
    const edited = Document("One\n");
    edited.edit("Two\n").document.close();
    assert.throws(() => edited.edit("Three\n"), /released/);

    const closed = Document("One\n");
    closed.close();
    // Closing twice is a no-op; editing after it is not.
    closed.close();
    assert.throws(() => closed.edit("Three\n"), /released/);
    // What was already extracted stays answerable either way.
    assert.equal(closed.scope(closed.content[0]).start.line, 1);
});

test("edits: diagnostics travel with the document that raised them", () => {
    // The one thing an editor underlines: a directive's `{...}` did not
    // parse, so the braces stayed literal text. Invisible in the tree — the
    // node simply has no attributes — which is why it is reported.
    const before = Document(":::note{= bad}\nbody\n:::\n");
    assert.deepEqual(
        before.diagnostics.map((diagnostic) => diagnostic.code),
        [DiagnosticCode.directiveAttributes]
    );
    assert.deepEqual(before.diagnostics[0].scope, {
        start: { line: 1, column: 8 },
        end: { line: 1, column: 14 }
    });

    const after = before.edit(":::note{a=1}\nbody\n:::\n").document;
    assert.equal(after.diagnostics.length, 0);
    // The predecessor keeps reporting its own: diagnostics are values it
    // copied out at parse time, like every other part of it.
    assert.equal(before.diagnostics.length, 1);
    after.close();
});

test("edits: options are fixed for a lineage and reported by every revision", () => {
    const before = Document("| a |\n| --- |\n| b |\n", { tables: false });
    assert.equal(before.options.tables, false);
    assert.equal(before.content[0].kind, "paragraph");
    const after = before.edit("| a |\n| --- |\n| c |\n").document;
    assert.equal(after.options.tables, false);
    assert.equal(after.content[0].kind, "paragraph");
    after.close();
});

test("edits: irregular render ticks over a multi-turn conversation", () => {
    // The shape of a real LLM consumer: every socket message extends the
    // text (nothing parses), only an irregular render tick edits, and the
    // messages between ticks conflate into that one edit. Three assistant
    // turns extend one document; blocks settled at a turn boundary must stay
    // frozen while later turns stream.
    const turns = [
        "# Streaming\n\nThe *quick* parser holds **steady** under bursts, " +
            "and the heading keeps its identity from the first render on.\n\n" +
            "Deltas stay proportional to what changed, so a renderer " +
            "reconciles by id instead of walking the whole tree.\n\n" +
            "> Snapshots are values: whatever a tick captured stays valid " +
            "while the socket races ahead.",
        "\n\n- append per message\n- edit per tick\n- settled blocks stay frozen" +
            "\n- identical items stress identity\n- identical items stress identity" +
            "\n\n```swift\nlet constant = 1\nlet mirror = [Int: String]()\n" +
            "for index in 0..<3 {\n    print(index, constant)\n}\n```\n\n" +
            "Fenced code arrives line by line and only closes at the final tick.",
        "\n\nA table lands late in the conversation:\n\n" +
            "| stage | commits | messages |\n| - | - | - |\n| one | 3 | 9 |\n" +
            "| two | 5 | 14 |\n| three | 8 | 21 |\n\n" +
            "Tail with a footnote[^n] whose definition arrives last.\n\n" +
            "[^n]: Resolved at the end, after every reference already rendered."
    ];
    // One fixed generator drives batch sizes and tick timing, so the burst
    // shapes are irregular but reproducible — and identical in the Swift and
    // Kotlin mirrors of this test.
    let state = 0x9e3779b97f4a7c15n;
    const draw = (bound) => {
        state = BigInt.asUintN(64, state * 6364136223846793005n + 1442695040888963407n);
        return Number((state >> 33n) % bound);
    };

    let document = Document("");
    let streamed = "";
    let frozen = [];
    let messages = 0;
    let ticks = 0;
    let touched = 0;

    const tick = () => {
        const commit = document.edit(streamed);
        document = commit.document;
        ticks += 1;
        touched += commit.delta.diffs.length;
        const reference = Document(streamed);
        assert.equal(document.dump(), reference.dump());
        reference.close();
        assertPostorder(commit.delta, document);
        for (const [index, id, revision] of frozen) {
            assert.equal(document.content[index].id, id);
            assert.equal(document.content[index].revision, revision);
        }
    };

    for (const turn of turns) {
        let offset = 0;
        while (offset < turn.length) {
            // Mostly a 20-30 token batch (80-150 characters), with
            // occasional tiny flushes of a few words. Cuts land at raw
            // character offsets — mid-word, mid-marker, even between the two
            // newlines of a block boundary — because that is the steady
            // state of LLM output.
            const width = draw(10n) < 2 ? 2 + draw(18n) : 80 + draw(71n);
            streamed += turn.slice(offset, offset + width);
            offset += width;
            messages += 1;
            if (draw(4n) === 0) tick();
        }
        // The turn boundary always renders; everything but the still-hot
        // last block is now settled.
        tick();
        frozen = document.content.slice(0, -1).map((node, index) => [index, node.id, node.revision]);
    }
    assert.ok(messages > 9);
    assert.ok(ticks < messages);
    const reference = Document(turns.join(""));
    assert.equal(document.dump(), reference.dump());
    reference.close();

    // Near-O(n) pipeline: total delta traffic stays within one row per final
    // node plus bounded frontier churn per tick. A full rebuild per tick
    // would be on the order of ticks * nodes.
    assert.ok(touched < flatten(document).length + 16 * ticks);
    document.close();
});
