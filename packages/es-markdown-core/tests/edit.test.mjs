import assert from "node:assert/strict";
import test from "node:test";
import { DiagnosticCode, Document, MarkupWalker, WalkEvent } from "../dist/index.js";

function flatten(document) {
    const nodes = [];
    new MarkupWalker().walk(document, (event, node) => {
        if (event === WalkEvent.entering) nodes.push(node);
    });
    return nodes;
}

/**
 * The nodes an edit stamped: everything in `successor` whose revision is
 * newer than the whole predecessor tree. The predecessor's root revision
 * bounds its every node, because a child's revision never exceeds its
 * parent's — so a node above that bound was stamped by this edit, and a node
 * at or below it was carried.
 */
function stamped(successor, predecessorRevision) {
    return flatten(successor).filter((node) => node.revision > predecessorRevision);
}

test("edits: extending the trailing text keeps its identity and bumps its revision", () => {
    const first = Document("# Title\n\nHello");
    const firstHeading = first.content[0];
    const firstParagraph = first.content[1];
    const firstText = firstParagraph.content[0];

    const second = first.edit("# Title\n\nHello world");
    const secondHeading = second.content[0];
    const secondParagraph = second.content[1];
    const secondText = secondParagraph.content[0];

    assert.equal(secondText.literal, "Hello world");
    assert.equal(secondParagraph.id, firstParagraph.id);
    assert.equal(secondText.id, firstText.id);
    assert.ok(secondText.revision > firstText.revision);
    // The heading settled before the edit: equal to its predecessor down to
    // the last field. It is not the SAME object — every node is decoded
    // every time — and nothing should need it to be: what a reactive
    // consumer compares is the (id, revision) pair.
    assert.equal(secondHeading.id, firstHeading.id);
    assert.equal(secondHeading.revision, firstHeading.revision);
    assert.deepEqual(secondHeading, firstHeading);

    // The edit stamped the text and everything above it — and only that.
    // The stamp is what says "changed"; the heading keeps its old revision.
    const touched = new Set(stamped(second, first.revision).map((node) => node.id));
    assert.equal(touched.has(secondHeading.id), false);
    assert.equal(touched.has(secondText.id), true);
    // Nothing was removed: every predecessor identity is still present.
    for (const node of flatten(first)) {
        assert.notEqual(second.node(node.id), null);
    }
    second.close();
});

test("edits: a clean-boundary insert at the top leaves downstream identity intact", () => {
    const before = Document("First\n\nSecond\n\nThird\n");
    const downstreamBefore = before.content.map((node) => [node.id, node.revision]);
    const thirdBefore = before.content[2];

    const after = before.edit("# New\n\nFirst\n\nSecond\n\nThird\n");

    assert.equal(after.content.length, 4);
    const inserted = after.content[0];
    after.content.slice(1).forEach((node, index) => {
        assert.equal(node.id, downstreamBefore[index][0]);
        assert.equal(node.revision, downstreamBefore[index][1]);
    });

    // A created node is a fresh identity carrying the new document revision.
    assert.ok(downstreamBefore.every(([id]) => id !== inserted.id));
    assert.equal(before.node(inserted.id), null);
    assert.equal(inserted.revision, after.revision);
    assert.ok(inserted.revision > before.revision);

    // Downstream nodes shifted by two lines: same identity, same revision,
    // new extent. Each value states where it is, so the predecessor's value
    // keeps reporting the predecessor's position forever — and the pair a
    // reactive consumer compares is unmoved by the shift.
    const third = after.content[3];
    assert.equal(third.scope.start.line, 7);
    assert.equal(thirdBefore.scope.start.line, 5);
    assert.equal(third.id, thirdBefore.id);
    assert.equal(third.revision, thirdBefore.revision);
    const reference = Document("# New\n\nFirst\n\nSecond\n\nThird\n");
    assert.equal(after.dump(), reference.dump());
    reference.close();
    after.close();
});

test("edits: a kind change retires the old identity and mints a new one", () => {
    const before = Document("text\n");
    const paragraph = before.content[0];

    const after = before.edit("# text\n");
    const heading = after.content[0];
    assert.notEqual(heading.id.rawValue, paragraph.id.rawValue);
    // The minted node carries the new document revision; the retired one is
    // simply gone from the tree.
    assert.equal(heading.revision, after.revision);
    assert.equal(after.node(paragraph.id), null);
    after.close();
});

test("edits: equality is series-salted identity plus revision", () => {
    const source = "Same *content* twice.\n";
    const first = Document(source);
    const second = Document(source);
    // Identical content from different parses never shares identity.
    assert.notEqual(first.id.series, second.id.series);
    assert.notEqual(first.content[0], second.content[0]);
    // An id from another series is not this document's to answer.
    assert.equal(first.node(second.content[0].id), null);
    assert.equal(first.node(first.content[0].id), first.content[0]);
    // The root answers for itself, like every node under it.
    assert.equal(first.node(first.id), first);
    first.close();
    second.close();
});

test("edits: a blank-line-only edit changes no node yet shifts scopes", () => {
    const before = Document("Alpha\n\n\n\nOmega\n");
    const omegaBefore = before.content[1];
    assert.equal(omegaBefore.scope.start.line, 5);
    const pairsBefore = flatten(before).map((node) => [node.id, node.revision]);

    // Delete two of the blank lines: no node's content changes, so every
    // node — the root included — keeps its exact identity and revision.
    // Position is not content, and revisions do not report it.
    const after = before.edit("Alpha\n\nOmega\n");
    assert.deepEqual(
        flatten(after).map((node) => [node.id, node.revision]),
        pairsBefore
    );
    assert.equal(after.content[1].id, omegaBefore.id);
    assert.equal(after.content[1].revision, omegaBefore.revision);
    assert.equal(after.content[1].scope.start.line, 3);
    const reference = Document("Alpha\n\nOmega\n");
    assert.equal(after.dump(), reference.dump());
    reference.close();
    after.close();
});

test("edits: a deep rebuild stamps the changed spine and nothing else", () => {
    const depth = 512;
    const stable = "Stable\n\n";
    const prefix = "> ".repeat(depth);
    const before = Document(stable + prefix + "alpha\n");
    const stableBefore = before.content[0];

    const after = before.edit(stable + prefix + "bravo\n");

    // The settled paragraph is untouched, so it survives byte for byte.
    assert.deepEqual(after.content[0], stableBefore);

    // One text changed, and a revision covers a node's whole subtree, so the
    // stamp reaches every ancestor: the innermost text, the chain of quotes,
    // and the root all carry the new document revision, while the settled
    // paragraph's subtree keeps its old one.
    let innermost = after.content[1];
    while (innermost.kind === "blockQuote") innermost = innermost.content[0];
    const text = innermost.content[0];
    assert.equal(text.literal, "bravo");
    assert.equal(text.revision, after.revision);
    const touched = stamped(after, before.revision);
    assert.ok(touched.length >= depth);
    const touchedIds = new Set(touched.map((node) => node.id));
    assert.equal(touchedIds.has(after.content[0].id), false);
    assert.equal(touchedIds.has(after.id), true);
    after.close();
});

test("edits: a superseded document keeps answering from its own values", () => {
    const first = Document("One\n\nTwo\n");
    const two = first.content[1];
    assert.equal(two.scope.start.line, 3);

    // The edit supersedes its receiver; here even the successor is closed at
    // once. The predecessor's values, scopes, diagnostics, and dump were all
    // extracted at parse time and owe nobody anything — decoded values live
    // forever.
    first.edit("Zero\n\nOne\n\nTwo\n").close();
    assert.equal(two.scope.start.line, 3);
    assert.ok(first.dump().includes("Paragraph"));
    assert.equal(flatten(first).length, 5);
    first.close();
});

test("edits: a mutation supersedes its receiver, so history is linear", () => {
    // A document is the live head of a chain. A mutation advances the chain
    // and supersedes its receiver: mutating the old head again is a
    // deterministic error, never a second line of descent.
    const base = Document("One\n");
    const head = base.edit("Two\n");
    assert.equal(head.content[0].content[0].literal, "Two");
    assert.throws(() => base.edit("Three\n"), /superseded/);
    assert.throws(() => base.append("Three\n"), /superseded/);
    // The rejected calls changed nothing: the real head still mutates, and
    // the superseded document still answers from its decoded values.
    assert.equal(base.content[0].content[0].literal, "One");
    const next = head.edit("Three\n");
    assert.equal(next.content[0].content[0].literal, "Three");
    assert.ok(next.revision > head.revision);
    base.close();
    head.close();
    next.close();

    const closed = Document("One\n");
    closed.close();
    // Closing twice is a no-op; editing after it is not.
    closed.close();
    assert.throws(() => closed.edit("Three\n"), /released/);
    // What was already extracted stays answerable either way.
    assert.equal(closed.content[0].scope.start.line, 1);
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

    const after = before.edit(":::note{a=1}\nbody\n:::\n");
    assert.equal(after.diagnostics.length, 0);
    // The predecessor keeps reporting its own: diagnostics are values it
    // copied out at parse time, like every other part of it.
    assert.equal(before.diagnostics.length, 1);
    after.close();
});

test("edits: options are fixed for a series and reported by every revision", () => {
    const before = Document("| a |\n| --- |\n| b |\n", { tables: false });
    assert.equal(before.options.tables, false);
    assert.equal(before.content[0].kind, "paragraph");
    const after = before.edit("| a |\n| --- |\n| c |\n");
    assert.equal(after.options.tables, false);
    assert.equal(after.content[0].kind, "paragraph");
    after.close();
});

test("edits: irregular render ticks over a multi-turn conversation", () => {
    // The shape of a real LLM consumer: every socket message APPENDS to the
    // document — the hot path — and an irregular render tick only verifies
    // what a renderer would draw. Three assistant turns extend one document;
    // blocks settled at a turn boundary must stay frozen while later turns
    // stream.
    const turns = [
        "# Streaming\n\nThe *quick* parser holds **steady** under bursts, " +
            "and the heading keeps its identity from the first render on.\n\n" +
            "Update traffic stays proportional to what changed, so a renderer " +
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
        ticks += 1;
        const reference = Document(streamed);
        assert.equal(document.dump(), reference.dump());
        reference.close();
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
            const chunk = turn.slice(offset, offset + width);
            streamed += chunk;
            offset += width;
            messages += 1;
            const previous = document;
            document = document.append(chunk);
            // What this append stamped, measured off the new tree itself:
            // the nodes carrying a fresher revision than the whole
            // predecessor tree — the same proxy the Swift and Kotlin mirrors
            // of this test count.
            touched += stamped(document, previous.revision).length;
            previous.close();
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

    // Near-O(n) pipeline: total stamped-node traffic stays within one node
    // per final node plus bounded frontier churn per append. A full rebuild
    // per message would be on the order of messages * nodes.
    assert.ok(touched < flatten(document).length + 24 * messages);
    document.close();
});
