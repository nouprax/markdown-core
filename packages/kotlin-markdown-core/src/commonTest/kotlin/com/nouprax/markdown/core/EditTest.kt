package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

class EditTest {
    @Test
    fun extendingTheTrailingTextKeepsItsIdentityAndBumpsItsRevision() {
        val first = Document("# Title\n\nHello")
        val firstHeading = assertIs<Heading>(first.content[0])
        val firstParagraph = assertIs<Paragraph>(first.content[1])
        val firstText = assertIs<Text>(firstParagraph.content[0])

        val commit = first.edit("# Title\n\nHello world")
        commit.document.use { second ->
            val secondHeading = assertIs<Heading>(second.content[0])
            val secondParagraph = assertIs<Paragraph>(second.content[1])
            val secondText = assertIs<Text>(secondParagraph.content[0])

            assertEquals("Hello world", secondText.literal)
            assertEquals(firstParagraph.id, secondParagraph.id)
            assertEquals(firstText.id, secondText.id)
            assertTrue(secondText.revision > firstText.revision)
            assertEquals<Markup>(firstHeading, secondHeading)

            // The heading settled before the edit and is not named at all;
            // the text it precedes is, and nothing is retired.
            val named =
                commit.delta.diffs
                    .map { it.markup }
                    .toSet()
            assertTrue(firstHeading.id !in named)
            assertTrue(secondText.id in named)
            assertTrue(commit.delta.diffs.none { it.parts.isRetired })
            assertTrue(
                commit.delta.diffs
                    .single { it.markup == secondText.id }
                    .parts.text,
            )
        }
    }

    @Test
    fun aCleanBoundaryInsertAtTheTopLeavesDownstreamIdentityIntact() {
        val before = Document("First\n\nSecond\n\nThird\n")
        val downstreamBefore = before.content.map { it.id to it.revision }
        val thirdBefore = assertIs<Paragraph>(before.content[2])

        val commit = before.edit("# New\n\nFirst\n\nSecond\n\nThird\n")
        commit.document.use { after ->
            assertEquals(4, after.content.size)
            val inserted = assertIs<Heading>(after.content[0])
            after.content.drop(1).forEachIndexed { index, node ->
                assertEquals(downstreamBefore[index].first, node.id)
                assertEquals(downstreamBefore[index].second, node.revision)
            }

            // A created node differs from absence in every part it has: a
            // heading has a value and children, and no text of its own.
            val insertedDiff = commit.delta.diffs.single { it.markup == inserted.id }
            assertTrue(insertedDiff.parts.value)
            assertTrue(insertedDiff.parts.children)
            assertTrue(insertedDiff.parts.descendant)
            assertTrue(!insertedDiff.parts.text)

            // Downstream nodes shifted by two lines: equal values, new scopes.
            assertEquals(7, after.scope(assertIs<Paragraph>(after.content[3])).start.line)
            // A value carried over from the predecessor has the same (id,
            // revision) and resolves against the successor at its NEW
            // position: identity survives the edit, absolute position does
            // not. And the predecessor still answers at its own.
            assertEquals(7, after.scope(thirdBefore).start.line)
            assertEquals(5, before.scope(thirdBefore).start.line)
            assertEquals(Document("# New\n\nFirst\n\nSecond\n\nThird\n").dump(), after.dump())
        }
    }

    @Test
    fun aKindChangeRetiresTheOldIdentityAndMintsANewOne() {
        val before = Document("text\n")
        val paragraph = assertIs<Paragraph>(before.content[0])

        val commit = before.edit("# text\n")
        commit.document.use { after ->
            val heading = assertIs<Heading>(after.content[0])
            assertNotEquals(paragraph.id, heading.id)

            val rows = commit.delta.diffs
            assertTrue(rows.any { it.markup == paragraph.id && it.parts.isRetired })
            assertTrue(rows.any { it.markup == heading.id && !it.parts.isRetired })
            // A retired node is emitted WHERE IT WAS FOUND — inside its former
            // parent's run, before what replaced it there and before that
            // parent's own row.
            assertTrue(
                rows.indexOfFirst { it.markup == paragraph.id } <
                    rows.indexOfFirst { it.markup == heading.id },
            )
            assertTrue(
                rows.indexOfFirst { it.markup == heading.id } <
                    rows.indexOfFirst { it.markup == after.id },
            )
            // The retired node is gone from the successor.
            assertNull(after.node(paragraph.id))
        }
    }

    @Test
    fun equalityIsLineageSaltedIdentityPlusRevision() {
        Document("Same *content* twice.\n").use { first ->
            Document("Same *content* twice.\n").use { second ->
                // Identical content from different parses never compares equal.
                assertNotEquals<Markup>(first, second)
                assertNotEquals<Markup>(first.content[0], second.content[0])
                // Within one document, identity is value equality.
                assertEquals(first.content[0], first.content[0])
                assertNotEquals(first.id.lineage, second.id.lineage)
                // An id from another lineage is not this document's to answer.
                assertNull(first.node(second.content[0].id))
            }
        }
    }

    @Test
    fun aBlankLineOnlyEditReportsAnEmptyDeltaYetShiftsScopes() {
        val before = Document("Alpha\n\n\n\nOmega\n")
        val omegaBefore = assertIs<Paragraph>(before.content[1])
        assertEquals(5, before.scope(omegaBefore).start.line)

        // Delete two of the blank lines: no node's content changes.
        val commit = before.edit("Alpha\n\nOmega\n")
        commit.document.use { after ->
            val omegaAfter = assertIs<Paragraph>(after.content[1])
            assertTrue(commit.delta.diffs.isEmpty())
            assertTrue(commit.delta.afterRevision > commit.delta.beforeRevision)
            assertEquals<Markup>(omegaBefore, omegaAfter)
            // Nothing was named, so the successor kept the predecessor's own
            // content list — the same objects, not equal copies.
            assertSame(omegaBefore, omegaAfter)
            assertEquals(3, after.scope(omegaAfter).start.line)
            assertEquals(Document("Alpha\n\nOmega\n").dump(), after.dump())
        }
    }

    @Test
    fun aDeepRebuildNamesChildrenBeforeParentsInOnePostorderPass() {
        val depth = 512
        val stable = "Stable\n\n"
        val prefix = "> ".repeat(depth)
        val before = Document(stable + prefix + "alpha\n")
        val stableBefore = assertIs<Paragraph>(before.content.first())

        val commit = before.edit(stable + prefix + "bravo\n")
        commit.document.use { after ->
            assertTrue(commit.delta.diffs.size >= depth)
            assertPostorder(commit.delta, after)
            assertEquals(stableBefore.id, after.content.first().id)
            assertEquals(Document(stable + prefix + "bravo\n").dump(), after.dump())

            // Exactly one node's own projection differs — the innermost text —
            // and every one of its ancestors carries `descendant` and nothing
            // else, which is what lets a renderer stop at the first node whose
            // own parts are all false.
            var innermost: Markup = after.content.last()
            while (innermost is BlockQuote) {
                innermost = innermost.content.first()
            }
            val text = assertIs<Text>(assertIs<Paragraph>(innermost).content.first())
            assertEquals("bravo", text.literal)
            val ownChanges = commit.delta.diffs.filter { it.parts.rawValue != 8 }
            assertEquals(listOf(text.id), ownChanges.map { it.markup })
            assertTrue(ownChanges.single().parts.text)
            // The chain, the paragraph it ends in, and the document above it.
            assertEquals(depth + 3, commit.delta.diffs.size)
        }
    }

    @Test
    fun aSupersededDocumentKeepsAnsweringFromItsOwnTables() {
        val first = Document("One\n\nTwo\n")
        val two = assertIs<Paragraph>(first.content[1])
        assertEquals(3, first.scope(two).start.line)

        // Editing hands the native parse to the successor. The predecessor's
        // values, scopes, diagnostics, and dump were all extracted at parse
        // time and owe that parse nothing.
        first.edit("Zero\n\nOne\n\nTwo\n").document.close()
        assertEquals(3, first.scope(two).start.line)
        assertTrue(first.dump().contains("Paragraph"))
        val recording = RecordingVisitor()
        MarkupWalker.walk(first, recording)
        assertEquals(5, recording.visited.size)
    }

    @Test
    fun anEditedOrClosedDocumentRefusesASecondEdit() {
        val edited = Document("One\n")
        edited.edit("Two\n").document.close()
        assertFailsWith<IllegalStateException> { edited.edit("Three\n") }

        val closed = Document("One\n")
        closed.close()
        // Closing twice is a no-op; editing after it is not.
        closed.close()
        assertFailsWith<IllegalStateException> { closed.edit("Three\n") }
        // What was already extracted stays answerable either way.
        assertEquals(1, closed.scope(closed.content[0]).start.line)
    }

    @Test
    fun documentsAndDeltasAreValuesAndIdsAreStableMapKeys() {
        Document("").use { empty ->
            assertTrue(empty.content.isEmpty())
            val commit = empty.edit("Alpha\n")
            commit.document.use { document ->
                val paragraph = assertIs<Paragraph>(document.content[0])
                val byId = hashMapOf(paragraph.id to "paragraph")
                assertEquals(paragraph.id, document.node(paragraph.id)?.id)
                // The root answers for itself, which is what a delta naming it
                // needs.
                assertEquals(document.id, document.node(document.id)?.id)
                assertEquals("paragraph", byId[paragraph.id])
                assertNull(document.node(MarkupID(document.lineage + 1UL, paragraph.id.rawValue)))
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

        before.edit(":::note{a=1}\nbody\n:::\n").document.use { after ->
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
            val commit = document.edit(streamed)
            document = commit.document
            ticks += 1
            touched += commit.delta.diffs.size
            assertEquals(Document(streamed).dump(), document.dump())
            assertPostorder(commit.delta, document)
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

        // Near-O(n) pipeline: total delta traffic stays within one row per
        // final node plus bounded frontier churn per tick. A full rebuild per
        // tick would be on the order of ticks * nodes.
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

/**
 * Asserts the delta's own ordering contract: every surviving node appears
 * after all of its own children, so a consumer materializing immutable values
 * bottom-up reads the list once, front to back.
 */
internal fun assertPostorder(
    delta: Delta,
    document: Document,
) {
    val position = mutableMapOf<MarkupID, Int>()
    delta.diffs.forEachIndexed { index, diff ->
        if (!diff.parts.isRetired) {
            position[diff.markup] = index
        }
    }
    val stack = ArrayDeque<MarkupID>()
    MarkupWalker.walk(document) { event, node, _ ->
        when (event) {
            WalkEvent.ENTERING -> {
                val parent = stack.lastOrNull()
                val above = parent?.let { position[it] }
                val below = position[node.id]
                if (above != null && below != null) {
                    assertTrue(below < above, "child ${node.id.rawValue} follows its parent")
                }
                stack.addLast(node.id)
            }

            WalkEvent.EXITING -> {
                stack.removeLast()
            }
        }
    }
}
