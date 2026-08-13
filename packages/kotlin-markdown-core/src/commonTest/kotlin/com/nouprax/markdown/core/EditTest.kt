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

        // Editing takes nothing from the predecessor. Its values, scopes,
        // diagnostics, and dump were all extracted at parse time and owe that
        // parse nothing.
        first.edit("Zero\n\nOne\n\nTwo\n").close()
        assertEquals(3, two.scope.start.line)
        assertTrue(first.dump().contains("Paragraph"))
        val recording = RecordingVisitor()
        MarkupWalker.walk(first, recording)
        assertEquals(5, recording.visited.size)
    }

    @Test
    fun aDocumentMayBeEditedTwiceAndOnlyAClosedOneRefuses() {
        // An edit READS its receiver, so the receiver survives it. Editing
        // one document twice gives two lines of descent: same predecessor,
        // same starting identities, different revisions.
        val base = Document("One\n")
        val first = base.edit("Two\n")
        val second = base.edit("Three\n")
        assertEquals("Two", assertIs<Text>(assertIs<Paragraph>(first.content[0]).content[0]).literal)
        assertEquals("Three", assertIs<Text>(assertIs<Paragraph>(second.content[0]).content[0]).literal)
        assertTrue(first.revision != second.revision)
        assertTrue(first.revision > base.revision && second.revision > base.revision)
        first.close()
        second.close()
        base.close()

        val closed = Document("One\n")
        closed.close()
        // Closing twice is a no-op; editing after it is not.
        closed.close()
        assertFailsWith<IllegalStateException> { closed.edit("Three\n") }
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
        // The shape of a real LLM consumer: every socket message extends the
        // text (nothing parses), only an irregular render tick edits, and the
        // messages between ticks conflate into that one edit. Three assistant
        // turns extend one document; blocks settled at a turn boundary must
        // stay frozen while later turns stream.
        val turns =
            listOf(
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
                    "[^n]: Resolved at the end, after every reference already rendered.",
            )
        // One fixed generator drives batch sizes and tick timing, so the
        // burst shapes are irregular but reproducible — and identical in the
        // Swift and ES mirrors of this test.
        var state = 0x9E3779B97F4A7C15UL.toLong()

        fun draw(bound: Long): Long {
            state = state * 6364136223846793005L + 1442695040888963407L
            return (state ushr 33) % bound
        }

        var document = Document("")
        var streamed = ""
        var frozen = emptyList<Triple<Int, MarkupID, ULong>>()
        var messages = 0
        var ticks = 0
        var touched = 0

        fun tick() {
            val predecessorRevision = document.revision
            document = document.edit(streamed)
            ticks += 1
            // The old delta row count, re-expressed on the tree: the nodes
            // whose revision equals the new document revision are exactly the
            // ones this tick changed or minted — and a root that kept its
            // revision means the tick changed nothing anywhere.
            if (document.revision != predecessorRevision) {
                val stamped = document.revision
                MarkupWalker.walk(document) { event, node, _ ->
                    if (event == WalkEvent.ENTERING && node.revision == stamped) {
                        touched += 1
                    }
                }
            }
            assertEquals(Document(streamed).dump(), document.dump())
            for ((index, id, revision) in frozen) {
                val node = document.content[index]
                assertEquals(id, node.id)
                assertEquals(revision, node.revision)
            }
        }

        for (turn in turns) {
            var offset = 0
            while (offset < turn.length) {
                // Mostly a 20-30 token batch (80-150 characters), with
                // occasional tiny flushes of a few words. Cuts land at raw
                // character offsets — mid-word, mid-marker, even between the
                // two newlines of a block boundary — because that is the
                // steady state of LLM output.
                val width = if (draw(10L) < 2L) 2 + draw(18L).toInt() else 80 + draw(71L).toInt()
                val message = turn.substring(offset, minOf(offset + width, turn.length))
                offset += message.length
                streamed += message
                messages += 1
                if (draw(4L) == 0L) {
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
        assertEquals(Document(turns.joinToString(separator = "")).dump(), document.dump())

        // Near-O(n) pipeline: the total count of nodes stamped with a new
        // document revision stays within one per final node plus bounded
        // frontier churn per tick. A full rebuild per tick would be on the
        // order of ticks * nodes.
        var nodes = 0
        MarkupWalker.walk(document) { event, _, _ ->
            if (event == WalkEvent.ENTERING) {
                nodes += 1
            }
        }
        assertTrue(touched < nodes + 16 * ticks)
        document.close()
    }
}
