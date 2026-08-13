package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class EditTest {
    @Test
    fun extendingTheTrailingTextKeepsItsIdentityAndBumpsItsRevision() {
        val first = Document("# Title\n\nHello")
        val firstHeading = assertIs<Heading>(first.content[0])
        val firstParagraph = assertIs<Paragraph>(first.content[1])
        val firstText = assertIs<Text>(firstParagraph.content[0])

        first.edit("# Title\n\nHello world").use { second ->
            val secondHeading = assertIs<Heading>(second.content[0])
            val secondParagraph = assertIs<Paragraph>(second.content[1])
            val secondText = assertIs<Text>(secondParagraph.content[0])

            assertEquals("Hello world", secondText.literal)
            assertEquals(firstParagraph.id, secondParagraph.id)
            assertEquals(firstText.id, secondText.id)
            assertTrue(secondText.revision > firstText.revision)
            assertEquals<Markup>(firstHeading, secondHeading)

            // The heading settled before the edit and keeps its exact
            // revision; the changed text and the paragraph above it carry the
            // new document revision — which is how "what changed" is asked of
            // the tree itself.
            assertEquals(firstHeading.revision, secondHeading.revision)
            assertEquals(second.revision, secondText.revision)
            assertEquals(second.revision, secondParagraph.revision)
        }
    }

    @Test
    fun aCleanBoundaryInsertAtTheTopLeavesDownstreamIdentityIntact() {
        val before = Document("First\n\nSecond\n\nThird\n")
        val downstreamBefore = before.content.map { it.id to it.revision }
        val thirdBefore = assertIs<Paragraph>(before.content[2])

        before.edit("# New\n\nFirst\n\nSecond\n\nThird\n").use { after ->
            assertEquals(4, after.content.size)
            val inserted = assertIs<Heading>(after.content[0])
            after.content.drop(1).forEachIndexed { index, node ->
                assertEquals(downstreamBefore[index].first, node.id)
                assertEquals(downstreamBefore[index].second, node.revision)
            }

            // A created node carries the new document revision, exactly like
            // everything else this edit minted or changed.
            assertEquals(after.revision, inserted.revision)

            // Downstream nodes shifted by two lines: equal values, new scopes.
            val third = assertIs<Paragraph>(after.content[3])
            assertEquals(7, third.scope.start.line)
            // The predecessor's value keeps the extent it was parsed with, and
            // the two are still EQUAL: identity survived the edit and position
            // is not part of equality.
            assertEquals(5, thirdBefore.scope.start.line)
            assertEquals<Markup>(thirdBefore, third)
            assertEquals(Document("# New\n\nFirst\n\nSecond\n\nThird\n").dump(), after.dump())
        }
    }

    @Test
    fun aKindChangeRetiresTheOldIdentityAndMintsANewOne() {
        val before = Document("text\n")
        val paragraph = assertIs<Paragraph>(before.content[0])

        before.edit("# text\n").use { after ->
            val heading = assertIs<Heading>(after.content[0])
            assertNotEquals(paragraph.id, heading.id)

            // The minted identity carries the new document revision, and
            // retirement is a presence question: the old id is simply no
            // longer this series' to answer.
            assertEquals(after.revision, heading.revision)
            assertNull(after.node(paragraph.id))
            // The predecessor still answers for it — retirement is the
            // successor's fact, not a rewrite of history.
            assertEquals(paragraph.id, before.node(paragraph.id)?.id)
        }
    }

    @Test
    fun equalityIsSeriesSaltedIdentityPlusRevision() {
        Document("Same *content* twice.\n").use { first ->
            Document("Same *content* twice.\n").use { second ->
                // Identical content from different parses never compares equal.
                assertNotEquals<Markup>(first, second)
                assertNotEquals<Markup>(first.content[0], second.content[0])
                // Within one document, identity is value equality.
                assertEquals(first.content[0], first.content[0])
                assertNotEquals(first.id.series, second.id.series)
                // An id from another series is not this document's to answer.
                assertNull(first.node(second.content[0].id))
            }
        }
    }

    @Test
    fun aBlankLineOnlyEditChangesNoRevisionYetShiftsScopes() {
        val before = Document("Alpha\n\n\n\nOmega\n")
        val omegaBefore = assertIs<Paragraph>(before.content[1])
        assertEquals(5, omegaBefore.scope.start.line)

        // Delete two of the blank lines: no node's content changes.
        before.edit("Alpha\n\nOmega\n").use { after ->
            val omegaAfter = assertIs<Paragraph>(after.content[1])
            // Nothing changed anywhere, so even the root kept its exact
            // revision — the tree's way of reporting an empty change set.
            assertEquals(before.revision, after.revision)
            assertEquals(before.id, after.id)
            // Equal, and at a different place: two nodes differing only in
            // where they sit are equal, because position is not content.
            assertEquals<Markup>(omegaBefore, omegaAfter)
            assertEquals(3, omegaAfter.scope.start.line)
            assertEquals(5, omegaBefore.scope.start.line)
            assertEquals(Document("Alpha\n\nOmega\n").dump(), after.dump())
        }
    }

    @Test
    fun aDeepRebuildStampsEveryAncestorOfTheOneChangedText() {
        val depth = 512
        val stable = "Stable\n\n"
        val prefix = "> ".repeat(depth)
        val before = Document(stable + prefix + "alpha\n")
        val stableBefore = assertIs<Paragraph>(before.content.first())

        before.edit(stable + prefix + "bravo\n").use { after ->
            assertEquals(stableBefore.id, after.content.first().id)
            assertEquals(stableBefore.revision, after.content.first().revision)
            assertEquals(Document(stable + prefix + "bravo\n").dump(), after.dump())

            // One text changed, and a revision covers the whole subtree below
            // its node: the changed text, the paragraph and every quote above
            // it, and the document itself carry the new document revision,
            // while the settled paragraph keeps its own — which is what lets
            // a renderer prune at the first node whose revision did not move.
            var innermost: Markup = after.content.last()
            while (innermost is BlockQuote) {
                assertEquals(after.revision, innermost.revision)
                innermost = innermost.content.first()
            }
            val text = assertIs<Text>(assertIs<Paragraph>(innermost).content.first())
            assertEquals("bravo", text.literal)
            assertEquals(after.revision, text.revision)
            // The chain, the paragraph it ends in, its text, and the document:
            // exactly depth + 3 nodes carry the new revision, and nothing else
            // does.
            var stamped = 0
            MarkupWalker.walk(after) { event, node, _ ->
                if (event == WalkEvent.ENTERING && node.revision == after.revision) {
                    stamped += 1
                }
            }
            assertEquals(depth + 3, stamped)
        }
    }

    @Test
    fun aSupersededDocumentKeepsAnsweringFromItsOwnTables() {
        val first = Document("One\n\nTwo\n")
        val two = assertIs<Paragraph>(first.content[1])
        assertEquals(3, two.scope.start.line)

        // The edit supersedes the receiver — mutation is now the successor's
        // alone — but supersession severs mutation, not the values. Content,
        // scopes, diagnostics, and dump were all extracted at parse time and
        // owe the native parse nothing, so the superseded document keeps
        // answering from its own tables.
        first.edit("Zero\n\nOne\n\nTwo\n").close()
        assertEquals(3, two.scope.start.line)
        assertTrue(first.dump().contains("Paragraph"))
        val recording = RecordingVisitor()
        MarkupWalker.walk(first, recording)
        assertEquals(5, recording.visited.size)
        first.close()
    }

    @Test
    fun aMutationSupersedesItsReceiverAndHistoryStaysLinear() {
        // A mutation advances the chain, old handles die, decoded values
        // live forever. There is no forking: mutating a superseded document
        // is a deterministic refusal with the engine's own wording, and the
        // refusal disturbs nothing.
        val base = Document("One\n")
        val head = base.edit("Two\n")
        for (refused in listOf<() -> Document>({ base.edit("Three\n") }, { base.append("!") })) {
            val refusal = assertFailsWith<IllegalStateException>(block = refused)
            assertEquals("the document has been superseded: mutate the successor", refusal.message)
        }
        // The superseded document still answers from its decoded values...
        assertEquals("One", assertIs<Text>(assertIs<Paragraph>(base.content[0]).content[0]).literal)
        // ...and the head is untouched by the refusals: it is still the live
        // head, and both mutations remain its to make.
        assertEquals("Two", assertIs<Text>(assertIs<Paragraph>(head.content[0]).content[0]).literal)
        val appended = head.append("Three\n")
        assertEquals(Document("Two\nThree\n").use { it.dump() }, appended.dump())
        assertTrue(appended.revision > head.revision)
        appended.close()
        head.close()
        base.close()

        val closed = Document("One\n")
        closed.close()
        // Closing twice is a no-op; mutating after it is not, and the closed
        // refusal is its own message — closing is the caller's act, not a
        // mutation's.
        closed.close()
        val refusal = assertFailsWith<IllegalStateException> { closed.edit("Three\n") }
        assertEquals("the document is closed", refusal.message)
        // What was already extracted stays answerable either way.
        assertEquals(
            1,
            closed.content[0]
                .scope.start.line,
        )
    }

    @Test
    fun documentsAreValuesAndIdsAreStableMapKeys() {
        Document("").use { empty ->
            assertTrue(empty.content.isEmpty())
            empty.edit("Alpha\n").use { document ->
                val paragraph = assertIs<Paragraph>(document.content[0])
                val byId = hashMapOf(paragraph.id to "paragraph")
                assertEquals(paragraph.id, document.node(paragraph.id)?.id)
                // The root answers for itself, which is what a consumer
                // reconciling by id needs when the top-level list changes.
                assertEquals(document.id, document.node(document.id)?.id)
                assertEquals("paragraph", byId[paragraph.id])
                assertNull(document.node(MarkupID(document.series + 1UL, paragraph.id.rawValue)))
            }
        }
    }

    @Test
    fun diagnosticsTravelWithTheDocumentThatRaisedThem() {
        // The one thing an editor underlines: a directive's `{...}` did not
        // parse, so the braces stayed literal text. Invisible in the tree —
        // the node simply has no attributes — which is why it is reported.
        val before = Document(":::note{= bad}\nbody\n:::\n")
        assertEquals(listOf(DiagnosticCode.DIRECTIVE_ATTRIBUTES), before.diagnostics.map { it.code })
        assertEquals(Scope(Position(1, 8), Position(1, 14)), before.diagnostics.single().scope)

        before.edit(":::note{a=1}\nbody\n:::\n").use { after ->
            assertTrue(after.diagnostics.isEmpty())
        }
        // The predecessor keeps reporting its own: diagnostics are values it
        // copied out at parse time, like every other part of it.
        assertEquals(1, before.diagnostics.size)
    }

    @Test
    fun streamingWithIrregularTicksOverAMultiTurnConversation() {
        // The shape of a real LLM consumer: every socket message rides the
        // real append — the streaming mutation — and an irregular render
        // tick reads what the appends produced. Three assistant turns extend
        // one document; blocks settled at a turn boundary must stay frozen
        // while later turns stream. The corpus text and the split generator
        // are the shared burst shapes, byte-identical in the Swift and ES
        // mirrors of this test.
        val splits = StreamingCorpus.Splits()

        var document = Document("")
        var streamed = ""
        var frozen = emptyList<Triple<Int, MarkupID, ULong>>()
        var messages = 0
        var ticks = 0
        var touched = 0

        fun tick() {
            ticks += 1
            // A tick no longer parses anything: the appends already did, and
            // the tick verifies their sum against a one-shot parse of every
            // byte so far.
            Document(streamed).use { reference ->
                assertEquals(reference.dump(), document.dump())
            }
            for ((index, id, revision) in frozen) {
                val node = document.content[index]
                assertEquals(id, node.id)
                assertEquals(revision, node.revision)
            }
        }

        for (turn in StreamingCorpus.turns) {
            var offset = 0
            while (offset < turn.length) {
                val width = splits.width()
                val message = turn.substring(offset, minOf(offset + width, turn.length))
                offset += message.length
                streamed += message
                messages += 1

                // Each message advances the chain; the superseded predecessor
                // is closed behind it — an O(1) release.
                val predecessorRevision = document.revision
                val superseded = document
                document = document.append(message)
                superseded.close()

                // The old delta row count, re-expressed on the tree: the
                // nodes whose revision equals the new document revision are
                // exactly the ones this append changed or minted — and a root
                // that kept its revision means the append changed nothing.
                if (document.revision != predecessorRevision) {
                    val stamped = document.revision
                    MarkupWalker.walk(document) { event, node, _ ->
                        if (event == WalkEvent.ENTERING && node.revision == stamped) {
                            touched += 1
                        }
                    }
                }
                if (splits.draw(4L) == 0L) {
                    tick()
                }
            }
            // The turn boundary always renders; everything but the still-hot
            // last block is now settled.
            tick()
            frozen =
                document.content.dropLast(1).mapIndexed { index, node ->
                    Triple(index, node.id, node.revision)
                }
        }
        assertTrue(messages > 9)
        assertTrue(ticks < messages)
        assertEquals(
            Document(StreamingCorpus.turns.joinToString(separator = "")).use { it.dump() },
            document.dump(),
        )

        // Near-O(n) pipeline: the total count of nodes stamped with a new
        // document revision stays within one per final node plus bounded
        // frontier churn per append. A full re-decode per append would be on
        // the order of messages * nodes — which is exactly the term the
        // pruned decode deletes.
        var nodes = 0
        MarkupWalker.walk(document) { event, _, _ ->
            if (event == WalkEvent.ENTERING) {
                nodes += 1
            }
        }
        assertTrue(touched < nodes + 16 * messages)
        document.close()
    }
}
