import assert from "node:assert/strict";
import { test } from "node:test";
import { Document, MarkupSession, MarkupWalker, WalkEvent } from "../dist/index.js";

test("sessions: streaming keeps frontier ids and bumps the trailing text revision", () => {
    const session = new MarkupSession();
    try {
        session.append("# Title\n\nHello");
        const first = session.commit();
        const firstHeading = first.document.content[0];
        const firstParagraph = first.document.content[1];
        const firstText = firstParagraph.content[0];

        session.append(" world");
        const second = session.commit();
        const secondHeading = second.document.content[0];
        const secondParagraph = second.document.content[1];
        const secondText = secondParagraph.content[0];

        assert.equal(secondText.literal, "Hello world");
        assert.equal(secondParagraph.id, firstParagraph.id);
        assert.equal(secondText.id, firstText.id);
        assert.ok(secondText.revision > firstText.revision);
        // An unchanged node is the same object across snapshots.
        assert.equal(secondHeading, firstHeading);
        assert.equal(second.delta.added.includes(secondParagraph.id), false);
        assert.equal(second.delta.removed.includes(firstText.id), false);
    } finally {
        session.close();
    }
});

test("sessions: a clean-boundary insert at the top leaves downstream identity intact", () => {
    const session = new MarkupSession();
    try {
        session.append("First\n\nSecond\n\nThird\n");
        const before = session.commit();
        const downstreamBefore = before.document.content.map((node) => [node.id, node.revision]);

        session.replace(0, 0, "# New\n\n");
        const after = session.commit();

        assert.equal(after.document.content.length, 4);
        const inserted = after.document.content[0];
        assert.equal(inserted.kind, "heading");
        assert.ok(after.delta.added.includes(inserted.id));
        after.document.content.slice(1).forEach((node, index) => {
            assert.equal(node.id, downstreamBefore[index][0]);
            assert.equal(node.revision, downstreamBefore[index][1]);
        });
        // Downstream nodes shifted by two lines: equal values, new scopes.
        const third = after.document.content[3];
        assert.equal(after.document.scope(third).start.line, 7);
        // An unchanged value carried over from the previous snapshot has the
        // same (id, revision) and resolves against the newer snapshot — at
        // its new position.
        const thirdBefore = before.document.content[2];
        assert.equal(after.document.scope(thirdBefore).start.line, 7);
        assert.equal(after.document.dump(), Document.parse("# New\n\nFirst\n\nSecond\n\nThird\n").dump());
    } finally {
        session.close();
    }
});

test("sessions: a kind change is reported as removed plus added", () => {
    const session = new MarkupSession();
    try {
        session.append("text\n");
        const before = session.commit();
        const paragraph = before.document.content[0];
        assert.equal(paragraph.kind, "paragraph");

        session.replace(0, 0, "# ");
        const after = session.commit();
        const heading = after.document.content[0];
        assert.equal(heading.kind, "heading");

        assert.ok(after.delta.removed.includes(paragraph.id));
        assert.ok(after.delta.added.includes(heading.id));
        assert.notEqual(paragraph.id, heading.id);
    } finally {
        session.close();
    }
});

test("sessions: a blank-line-only edit commits an empty delta yet shifts scopes", () => {
    const session = new MarkupSession();
    try {
        session.append("Alpha\n\n\n\nOmega\n");
        const before = session.commit();
        const omegaBefore = before.document.content[1];
        assert.equal(before.document.scope(omegaBefore).start.line, 5);

        // Delete two of the blank lines: no node's content changes.
        session.replace(6, 8, "");
        const after = session.commit();
        const omegaAfter = after.document.content[1];

        assert.deepEqual(after.delta.added, []);
        assert.deepEqual(after.delta.removed, []);
        assert.deepEqual(after.delta.changed, []);
        assert.deepEqual(after.delta.bubbled, []);
        assert.equal(omegaAfter, omegaBefore);
        assert.equal(after.document.scope(omegaAfter).start.line, 3);
        assert.equal(after.document.dump(), Document.parse("Alpha\n\nOmega\n").dump());
    } finally {
        session.close();
    }
});

test("sessions: materialized scopes survive the session and later commits", () => {
    const session = new MarkupSession();
    session.append("One\n\nTwo\n");
    const first = session.commit();
    const two = first.document.content[1];
    // Materialize while current.
    assert.equal(first.document.scope(two).start.line, 3);

    session.replace(0, 0, "Zero\n\n");
    session.commit();
    // The superseded snapshot answers from its cache, at its revision.
    assert.equal(first.document.scope(two).start.line, 3);

    session.close();
    assert.equal(first.document.scope(two).start.line, 3);
});

test("sessions: a superseded snapshot that never materialized fails instead of guessing", () => {
    const session = new MarkupSession();
    try {
        session.append("One\n");
        const first = session.commit();
        session.append("\nTwo\n");
        session.commit();
        assert.throws(() => first.document.scope(first.document.content[0]), /superseded snapshot/);
    } finally {
        session.close();
    }
});

test("sessions: footnote queries answer numbering, resolution, and back-references", () => {
    const session = new MarkupSession();
    try {
        session.append("See [^b] then [^a].\n\n[^a]: A\n\n[^b]: B\n");
        const commit = session.commit();

        const footnotes = session.footnotes();
        assert.deepEqual(
            footnotes.map((definition) => definition.label),
            ["b", "a"]
        );

        const definitionA = footnotes.at(-1);
        const infoA = session.footnote(definitionA.id);
        assert.equal(infoA.number, 2);
        assert.equal(infoA.definition, definitionA.id);
        assert.equal(infoA.referenceCount, 1);
        assert.equal(infoA.referenceOrdinal, null);

        const references = session.references(definitionA.id);
        assert.deepEqual(
            references.map((reference) => reference.label),
            ["a"]
        );
        const referenceInfo = session.footnote(references[0].id);
        assert.equal(referenceInfo.number, 2);
        assert.equal(referenceInfo.referenceOrdinal, 1);

        // A non-footnote id answers null.
        assert.equal(session.footnote(commit.document.id), null);

        // An ordinal shift surfaces as changed entries with identical dump
        // content.
        session.replace(0, 0, "Lead [^a].\n\n");
        const shifted = session.commit();
        assert.deepEqual(
            session.footnotes().map((definition) => definition.label),
            ["a", "b"]
        );
        assert.ok(shifted.delta.changed.length > 0);
    } finally {
        session.close();
    }
});

test("sessions: conflated streaming with irregular ticks over a multi-turn conversation", () => {
    // The shape of a real LLM consumer: every socket message appends
    // (nothing parses), only an irregular render tick commits, and the
    // messages between ticks conflate into that one commit. Three assistant
    // turns extend one document; blocks settled at a turn boundary must stay
    // frozen while later turns stream.
    const turns = [
        "# Streaming\n\nThe *quick* parser holds **steady** under bursts, " +
            "and the heading keeps its identity from the first render on.\n\n" +
            "Deltas stay proportional to what changed, so a renderer " +
            "reconciles by id instead of walking the whole tree.\n\n" +
            "> Snapshots are values: whatever a tick captured stays valid " +
            "while the socket races ahead.",
        "\n\n- append per message\n- commit per tick\n- settled blocks stay frozen" +
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
    const mask = (1n << 64n) - 1n;
    let state = 0x9e3779b97f4a7c15n;
    function draw(bound) {
        state = (state * 6364136223846793005n + 1442695040888963407n) & mask;
        return Number((state >> 33n) % bound);
    }
    const session = new MarkupSession();
    try {
        let streamed = "";
        let frozen = [];
        let messages = 0;
        let commits = 0;
        let touched = 0;
        function tick() {
            const commit = session.commit();
            commits += 1;
            touched +=
                commit.delta.added.length +
                commit.delta.removed.length +
                commit.delta.changed.length +
                commit.delta.bubbled.length;
            assert.equal(commit.document.dump(), Document.parse(streamed).dump());
            for (const [index, id, revision] of frozen) {
                const node = commit.document.content[index];
                assert.equal(node.id, id);
                assert.equal(node.revision, revision);
            }
        }

        for (const turn of turns) {
            let offset = 0;
            while (offset < turn.length) {
                // Mostly a 20-30 token batch (80-150 characters), with
                // occasional tiny flushes of a few words. Cuts land at raw
                // character offsets — mid-word, mid-marker, even between
                // the two newlines of a block boundary — because that is
                // the steady state of LLM output.
                const width = draw(10n) < 2 ? 2 + draw(18n) : 80 + draw(71n);
                const message = turn.slice(offset, offset + width);
                offset += message.length;
                session.append(message);
                streamed += message;
                messages += 1;
                if (draw(4n) === 0) tick();
            }
            // The turn boundary always renders; everything but the still-hot
            // last block is now settled.
            tick();
            frozen = session.document.content.slice(0, -1).map((node, index) => [index, node.id, node.revision]);
        }
        assert.ok(messages > 9);
        assert.ok(commits < messages);
        assert.equal(session.document.dump(), Document.parse(turns.join("")).dump());

        // Near-O(n) pipeline: total delta traffic stays within one add per
        // final node plus bounded frontier churn per tick. A full rebuild
        // per tick would be on the order of commits * nodes.
        let nodes = 0;
        new MarkupWalker().walk(session.document, (event) => {
            if (event === WalkEvent.entering) nodes += 1;
        });
        assert.ok(touched < nodes + 16 * commits);
    } finally {
        session.close();
    }
});

test("sessions: snapshots are values and ids are stable map keys", () => {
    const session = new MarkupSession();
    try {
        assert.deepEqual(session.document.content, []);
        assert.equal(session.revision, 0);
        session.append("Alpha\n");
        const commit = session.commit();
        assert.equal(session.revision, 1);
        assert.equal(session.length, 6);
        const paragraph = commit.document.content[0];
        // The same identity is always the same object, so ids key maps.
        const byId = new Map([[paragraph.id, "paragraph"]]);
        assert.equal(session.node(paragraph.id), paragraph);
        assert.equal(byId.get(paragraph.id), "paragraph");
        assert.equal(session.node({ lineage: session.lineage + 1n, rawValue: paragraph.id.rawValue }), null);
    } finally {
        session.close();
    }
});

test("sessions: a closed session keeps snapshots but refuses new work", () => {
    const session = new MarkupSession();
    session.append("Alpha\n");
    const commit = session.commit();
    commit.document.scope(commit.document.content[0]);
    session.close();
    session.close();
    assert.equal(commit.document.content[0].content[0].literal, "Alpha");
    assert.equal(commit.document.scope(commit.document.content[0]).start.line, 1);
    assert.throws(() => session.append("beta"), /closed/);
    assert.throws(() => session.commit(), /closed/);
    assert.throws(() => session.footnotes(), /closed/);
    assert.throws(() => session.revision, /closed/);
});

test("sessions: invalid edit ranges are rejected", () => {
    const session = new MarkupSession();
    try {
        assert.throws(() => session.replace(2, 1, "x"), RangeError);
        assert.throws(() => session.replace(-1, 1, "x"), RangeError);
        assert.throws(() => session.append(42), TypeError);
        session.append("abc");
        // Offsets at or beyond 2^32 would wrap at the 32-bit WASM boundary
        // (here to [0, 0), an insert at the start) instead of failing the
        // native length check; they must be rejected before the crossing.
        assert.throws(() => session.replace(2 ** 32, 2 ** 32, "x"), RangeError);
        assert.throws(() => session.replace(0, 2 ** 32 + 3, "x"), RangeError);
        assert.throws(
            () => session.replace(1, 9, "x"),
            (error) => error.name === "ParseError" && error.code === "invalidArgument"
        );
        // The session stays usable after a rejected edit.
        session.commit();
        assert.equal(session.document.dump(), Document.parse("abc").dump());
    } finally {
        session.close();
    }
});

test("sessions: options gate extensions for the session lifetime", () => {
    const markdown = "| a |\n| --- |\n| b |\n";
    const tabled = new MarkupSession();
    const plain = new MarkupSession({ tables: false });
    try {
        tabled.append(markdown);
        plain.append(markdown);
        assert.equal(tabled.commit().document.content[0].kind, "table");
        assert.equal(plain.commit().document.content[0].kind, "paragraph");
        assert.equal(plain.options.tables, false);
        assert.equal(plain.options.footnotes, true);
        assert.throws(() => new MarkupSession({ tables: "yes" }), TypeError);
    } finally {
        tabled.close();
        plain.close();
    }
});

test("sessions: interleaved sessions in one context stay independent", () => {
    // Zero process-global state: many live sessions per context, advanced in
    // an interleaved order with disagreeing options, never observe each
    // other.
    const sources = [
        "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
        "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
        "Formula $x^2$ inline and *a$b*c$ flanking.\n\n$$\nx = y\n$$\n"
    ];
    const options = [undefined, { tables: false, strikethrough: false }, { formulas: false }];
    const jobs = sources.flatMap((source) => options.map((variant) => ({ source, variant })));
    const sessions = jobs.map(({ variant }) => new MarkupSession(variant));
    try {
        const lines = jobs.map(({ source }) => source.split(/(?<=\n)/));
        for (let line = 0; lines.some((chunks) => line < chunks.length); line += 1) {
            jobs.forEach((_, index) => {
                const chunk = lines[index][line];
                if (chunk !== undefined) {
                    sessions[index].append(chunk);
                    sessions[index].commit();
                }
            });
        }
        jobs.forEach(({ source, variant }, index) => {
            assert.equal(sessions[index].document.dump(), Document.parse(source, variant).dump());
        });
        const lineages = new Set(sessions.map((session) => session.lineage));
        assert.equal(lineages.size, sessions.length);
    } finally {
        for (const session of sessions) session.close();
    }
});

test("sessions: worker threads replay sessions on isolated engine instances", async () => {
    // Each worker imports the module fresh (its own WASM instance and
    // per-session scratch) and replays the jobs through sessions; the dumps
    // must reproduce the main thread's one-shot references byte-for-byte.
    const { Worker } = await import("node:worker_threads");
    const jobs = [
        { source: "# Heading\n\nBody with *emphasis*.\n", options: undefined },
        { source: "| a |\n| --- |\n| b |\n\nSee [^n].\n\n[^n]: note\n", options: undefined },
        { source: "| a |\n| --- |\n| b |\n", options: { tables: false } }
    ];
    const references = jobs.map(({ source, options }) => Document.parse(source, options).dump());
    const workers = Array.from(
        { length: 4 },
        () =>
            new Promise((resolve, reject) => {
                const worker = new Worker(new URL("./worker-session.mjs", import.meta.url), {
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
