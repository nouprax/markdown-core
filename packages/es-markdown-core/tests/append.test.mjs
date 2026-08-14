import assert from "node:assert/strict";
import test from "node:test";
import { Document, MarkupWalker, WalkEvent } from "../dist/index.js";
import { unprunedDecode } from "../dist/document.js";

function flatten(document) {
    const nodes = [];
    new MarkupWalker().walk(document, (event, node) => {
        if (event === WalkEvent.entering) nodes.push(node);
    });
    return nodes;
}

/**
 * The value-mirror gate, applied to one append step: the pruned decode the
 * public `append` produced must deep-equal an independent, mirror-free
 * decode of the same native document — field by field, literals and scopes
 * included, never by object identity. This is the only oracle that can see
 * a reuse defect: the dump and the revision ledger are both blind to a
 * value bit wrongly carried whole.
 *
 * The bonus claim rides along: any node whose (id, revision) pair AND
 * extent survived the append IS the predecessor's value object, `===` —
 * that is what "per-tick O(changed) decode" hands a reactive consumer. The
 * extent belongs in the claim because revision covers content, not
 * position, and an append can move a sub-line boundary on the hot line
 * without touching content — a growing table row's earlier cell gains a
 * trailing padding column the moment the next delimiter arrives, and that
 * node is honestly re-decoded.
 */
function verifyMirror(successor, predecessor, label) {
    const oracle = unprunedDecode(successor);
    assert.deepStrictEqual(successor, oracle, label);

    const successors = flatten(successor);
    const before = new Map(flatten(predecessor).map((node) => [node.id.rawValue, node]));
    for (const node of successors) {
        // The root is the document value itself: re-adopted every mutation
        // by design, because it is what carries the successor's mediators.
        if (node === successor) continue;
        const kept = before.get(node.id.rawValue);
        if (kept === undefined || kept.revision !== node.revision) continue;
        if (JSON.stringify(kept.scope) !== JSON.stringify(node.scope)) continue;
        assert.ok(node === kept, `${label}: an unchanged (id, revision) node was re-decoded`);
    }
    // Walk repopulation, not eviction: every id the new tree presents is
    // answerable, and every id it does not present is null.
    const presented = new Set(successors.map((node) => node.id));
    for (const node of successors) {
        assert.equal(successor.node(node.id), node, label);
    }
    for (const node of before.values()) {
        if (successor.node(node.id) === null) {
            assert.ok(!presented.has(node.id), label);
        }
    }
}

test("appends: the trailing mutation extends the text and keeps settled identity", () => {
    const first = Document("# Title\n\nHello");
    const heading = first.content[0];
    const paragraph = first.content[1];
    const text = paragraph.content[0];

    const second = first.append(" world");
    assert.equal(second.content[1].content[0].literal, "Hello world");
    assert.equal(second.content[1].id, paragraph.id);
    assert.equal(second.content[1].content[0].id, text.id);
    assert.ok(second.content[1].content[0].revision > text.revision);
    // The heading settled before the append, so the successor's heading is
    // the predecessor's very value object — not a re-decoded equal.
    assert.ok(second.content[0] === heading);
    verifyMirror(second, first, "trailing append");

    // Same tree as a one-shot parse of the concatenated text.
    const reference = Document("# Title\n\nHello world");
    assert.equal(second.dump(), reference.dump());
    reference.close();
    first.close();
    second.close();
});

test("appends: per-code-point appends land on the one-shot parse", () => {
    // A JavaScript chunk is a string, and the boundary re-encodes each
    // chunk as valid UTF-8 — a mid-UTF-8 byte split is not expressible from
    // here, so the engine's own replay gates carry that class. A string CAN
    // still tear a surrogate pair; the one-unit hold-back that heals that
    // split is pinned by the unicode suite. Here the split is per code
    // point: the rocket keeps its four bytes together while every marker
    // around it is torn apart.
    const source = "para *em🚀ph* tail\n";
    const whole = Document(source);
    let document = Document("");
    for (const unit of source) {
        const previous = document;
        document = document.append(unit);
        verifyMirror(document, previous, `code point ${unit.codePointAt(0)}`);
        previous.close();
    }
    assert.equal(document.dump(), whole.dump());
    whole.close();
    document.close();
});

test("appends: a trailing terminator cannot smuggle a stale scope through the mirror", () => {
    // Position is not content: appending a terminating newline to a
    // document that ended without one may move the trailing construct's
    // scope END without stamping its revision (the same class as the
    // growing table row's cell padding). Whether this engine build absorbs
    // the terminator or not, the pruned decode must report exactly the
    // scope a mirror-free decode of the same native document reads — the
    // reuse condition is (id, revision, extent), never the pair alone.
    const before = Document("para\n\n---");
    const breakBefore = before.content[1];
    assert.equal(breakBefore.kind, "thematicBreak");

    const after = before.append("\n");
    const breakAfter = after.node(breakBefore.id);
    assert.notEqual(breakAfter, null);
    assert.equal(breakAfter.revision, breakBefore.revision);
    const oracle = unprunedDecode(after);
    assert.deepStrictEqual(breakAfter.scope, oracle.content[1].scope);
    // The bonus claim on a reuse hit: if the extent held too, the node IS
    // the predecessor's value object.
    if (JSON.stringify(breakAfter.scope) === JSON.stringify(breakBefore.scope)) {
        assert.ok(breakAfter === breakBefore);
    }
    verifyMirror(after, before, "trailing terminator");
    before.close();
    after.close();
});

test("appends: an empty append advances the chain and changes no value", () => {
    const base = Document("# Title\n\nHello\n");
    const pairs = flatten(base).map((node) => [node.id, node.revision]);

    const idle = base.append("");
    // Deep-equal tree: same nodes, same revisions, same projection — and
    // the content is the predecessor's very objects, not copies.
    assert.deepEqual(
        flatten(idle).map((node) => [node.id, node.revision]),
        pairs
    );
    assert.equal(idle.dump(), base.dump());
    idle.content.forEach((node, index) => assert.ok(node === base.content[index]));
    verifyMirror(idle, base, "empty append");

    // The chain still advanced: the receiver is superseded...
    assert.throws(() => base.append("!"), /superseded/);
    // ...and the empty append consumed a document revision of its own: the
    // next change stamps two past the base root, not one.
    const changed = idle.append("!\n");
    assert.equal(changed.revision, base.revision + 2);
    base.close();
    idle.close();
    changed.close();
});

test("appends: a rejected argument fails the call, and the receiver stays the head", () => {
    const document = Document("One");
    // Argument failures supersede nothing and poison nothing.
    assert.throws(() => document.append(1), TypeError);
    const next = document.append(" more\n");
    assert.equal(next.content[0].content[0].literal, "One more");
    // A superseded handle supports close — idempotently — and nothing else
    // that needs the native parse.
    document.close();
    document.close();
    const tail = next.append(" tail\n");
    assert.throws(() => unprunedDecode(next), /superseded/);
    tail.close();
    next.close();
});

test("appends: a closed document refuses to append", () => {
    const closed = Document("One\n");
    closed.close();
    assert.throws(() => closed.append("Two\n"), /released/);
    assert.equal(closed.content[0].scope.start.line, 1);
});

test("appends: the value mirror survives a whole streamed conversation", () => {
    // The shared three-turn corpus, byte-identical to the Swift and Kotlin
    // mirrors of the streaming tests, streamed at token-sized strides: cuts
    // land mid-word, mid-marker, and between the two newlines of a block
    // boundary. After EVERY append the pruned decode is compared field by
    // field against a mirror-free decode of the same native document.
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
    const source = turns.join("");
    let state = 0x9e3779b97f4a7c15n;
    const draw = (bound) => {
        state = BigInt.asUintN(64, state * 6364136223846793005n + 1442695040888963407n);
        return Number((state >> 33n) % bound);
    };

    let document = Document("");
    let streamed = "";
    let offset = 0;
    let appends = 0;
    while (offset < source.length) {
        // Token-sized strides of 3-8 characters — the split class per-line
        // replay is blind to, and the primary gate of the streaming plan.
        const width = 3 + draw(6n);
        const chunk = source.slice(offset, offset + width);
        streamed += chunk;
        offset += width;
        appends += 1;
        const previous = document;
        document = document.append(chunk);
        verifyMirror(document, previous, `append ${appends}`);
        previous.close();

        const reference = Document(streamed);
        assert.equal(document.dump(), reference.dump(), `append ${appends}`);
        reference.close();
    }
    assert.ok(appends > 100);
    const reference = Document(source);
    assert.equal(document.dump(), reference.dump());
    reference.close();
    document.close();
});

test("appends: identity objects stay interned across a whole chain of appends", () => {
    // Ids remain === across the whole series: the interning map must keep
    // full coverage through pruned decodes, or a later decode would mint
    // duplicate MarkupID objects for nodes that were carried whole.
    const first = Document("# Title\n\nSettled *deep* inline.\n\nTail");
    const deepIds = flatten(first).map((node) => node.id);

    const second = first.append(" grows");
    const third = second.append(" more");
    for (const id of deepIds) {
        const survivor = third.node(id);
        if (survivor === null) continue;
        assert.ok(survivor.id === id, "a surviving node re-minted its MarkupID object");
    }
    first.close();
    second.close();
    third.close();
});
