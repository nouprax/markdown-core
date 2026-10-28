package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

private fun place(
    start: Pair<Int, Int>,
    end: Pair<Int, Int>,
) = Scope(Position(start.first, start.second), Position(end.first, end.second))

/** Whether [call] throws the library's error with [ErrorCode.OUT_OF_BOUNDS]. */
private fun assertOutOfBounds(call: () -> Any?) {
    assertEquals(ErrorCode.OUT_OF_BOUNDS, assertFailsWith<MarkdownCoreException> { call() }.code)
}

/** A session's context, path, field and marker queries, and the dialect's names. */
class ContextTest {
    @Test
    fun anOpenCrossLinkTargetIsADestSlotThatSpellsItsCloserAfterTheCaret() {
        MarkdownSession("[[No\n").use { session ->
            session.locate(Position(1, 5))
            val context = session.context
            assertEquals(listOf(NodeKind.PARAGRAPH), context.blocks.map { it.kind })
            val slot = context.slots.single()
            assertEquals(SlotKind.DEST, slot.kind)
            assertEquals(NodeKind.CROSS_LINK, slot.owner)
            assertFalse(slot.closed)
            assertTrue(slot.prefix.isEmpty())
            assertEquals(place(1 to 3, 1 to 4), slot.before)
            assertEquals(place(1 to 5, 1 to 4), slot.after)
            val spelled = session.spell(slot, "x")
            assertEquals("x]]", spelled?.text)
            assertEquals(1, spelled?.cursor)
            // A `|` would end the target, and the field holds it in no spelling.
            assertNull(session.spell(slot, "a|b"))
            assertEquals(listOf(spelled, null), session.spell(slot, listOf("x", "a|b")))
        }
    }

    @Test
    fun aSpellingCountsItsCaretInTheSessionsUnit() {
        for ((unit, cursor) in listOf(TextUnit.UTF8 to 6, TextUnit.UTF16 to 3)) {
            MarkdownSession("[[No\n", unit).use { session ->
                session.locate(Position(1, 5))
                val slot = session.context.slots.single()
                assertEquals(cursor, session.spell(slot, "é🚀")?.cursor)
            }
        }
    }

    @Test
    fun aClosedConstructsSlotReadsItsFieldOnPastTheCursor() {
        MarkdownSession("[[Note]] after\n").use { session ->
            session.locate(Position(1, 5))
            val slot = session.context.slots.single()
            assertTrue(slot.closed)
            assertEquals(place(1 to 5, 1 to 6), slot.after)
            assertEquals(Slot.Insertion("x", 1), session.spell(slot, "x"))
        }
    }

    @Test
    fun theContextIsTheSessionsStateThatALocateReplacesAndAnAppendClears() {
        val session = MarkdownSession("[[No")
        assertTrue(session.context.blocks.isEmpty() && session.context.slots.isEmpty())
        session.locate(Position(1, 5))
        val slot = session.context.slots.single()
        session.locate(Position(1, 1))
        assertTrue(session.context.slots.isEmpty())
        assertOutOfBounds { session.spell(slot, "x") }
        session.locate(Position(1, 5))
        val next = session.context.slots.single()
        session.append("te]]\n")
        assertTrue(session.context.slots.isEmpty())
        assertOutOfBounds { session.spell(next, "x") }
        session.locate(Position(1, 5))
        val last = session.context.slots.single()
        assertTrue(last.closed)
        assertOutOfBounds { session.locate(Position(9, 1)) }
        assertEquals("x", session.spell(last, "x")?.text)
        // Another session at the same turn holds a slot at the same place.
        MarkdownSession("[[No").use { other ->
            repeat(3) { other.locate(Position(1, 1)) }
            other.append("te]]\n")
            other.locate(Position(1, 5))
            assertEquals(1, other.context.slots.size)
            assertOutOfBounds { other.spell(last, "x") }
        }
        session.close()
        assertFailsWith<IllegalStateException> { session.spell(last, "x") }
        assertFailsWith<IllegalStateException> { session.locate(Position(1, 1)) }
    }

    @Test
    fun aCursorOutsideTheTextOrBelowOneIsOutOfBoundsAndKeepsTheContext() {
        MarkdownSession("a\n").use { session ->
            session.locate(Position(1, 2))
            for (position in listOf(Position(0, 1), Position(1, 0), Position(3, 1))) {
                assertOutOfBounds { session.locate(position) }
                assertEquals(listOf(NodeKind.PARAGRAPH), session.context.blocks.map { it.kind })
            }
            session.locate(Position(2, 1))
            assertTrue(session.context.slots.isEmpty())
            assertOutOfBounds { session.path(Position(0, 1)) }
        }
    }

    @Test
    fun thePathHoldsEveryNodeOverAByteRootFirstAndEndsAtTheNodeThere() {
        val source = "> a *b*\n"
        MarkdownSession(source).use { session ->
            val document = session.finalize()
            for (column in 1..8) {
                val position = Position(1, column)
                val path = session.path(position)
                assertEquals(document.node(position, source)?.id, path.lastOrNull()?.id)
                assertTrue(path.isEmpty() || path.first().id == document.id)
            }
            assertEquals(5, session.path(Position(1, 6)).size)
            assertTrue(session.path(Position(2, 1)).isEmpty())
        }
    }

    @Test
    fun aLinksFieldsAndMarkersDivideItsOwnSource() {
        MarkdownSession("[a](doc.md#part)\n").use { session ->
            val document = session.finalize()
            val link = assertIs<Link>(assertIs<Paragraph>(document.content.first()).content.first())
            assertEquals(listOf(place(1 to 5, 1 to 10)), session.scope(link, SlotKind.DEST))
            assertEquals(listOf(place(1 to 12, 1 to 15)), session.scope(link, SlotKind.ANCHOR))
            assertTrue(session.scope(link, SlotKind.LABEL).isEmpty())
            val markers = session.markers(link)
            assertEquals(place(1 to 1, 1 to 1), markers.first())
            assertEquals(place(1 to 16, 1 to 16), markers.last())
            // The text the link holds is its child's, not the link's own.
            assertFalse(place(1 to 2, 1 to 2) in markers)
        }
    }

    @Test
    fun aNodeTheSessionsDocumentDoesNotHoldIsOutOfBounds() {
        val other = Document.parse("[a](doc.md#part)\n")
        val link = assertIs<Link>(assertIs<Paragraph>(other.content.first()).content.first())
        MarkdownSession("x\n\n[a](other.md#part)\n").use { session ->
            assertOutOfBounds { session.scope(link, SlotKind.DEST) }
            assertOutOfBounds { session.markers(link) }
        }
    }

    @Test
    fun anAppendKeepsAnsweringForTheNodesItLeavesUnchangedAndNotForThoseItChanges() {
        MarkdownSession("[a](doc.md)\n\nb").use { session ->
            val before = session.document
            val link = assertIs<Link>(assertIs<Paragraph>(before.content.first()).content.first())
            val text = assertIs<Text>(assertIs<Paragraph>(before.content.last()).content.first())
            session.append("c\n")
            assertEquals(listOf(place(1 to 5, 1 to 10)), session.scope(link, SlotKind.DEST))
            assertOutOfBounds { session.markers(text) }
        }
    }

    @Test
    fun aSessionsSnapshotSeesNoDefinitionAfterTheAskingRoot() {
        MarkdownSession("[a]\n\n").use { session ->
            val document = session.append("[a]: /url\n")
            val text = assertIs<Text>(assertIs<Paragraph>(document.content.first()).content.first())
            assertEquals(text.id, session.path(Position(1, 1)).last().id)
            assertTrue(session.markers(text).isEmpty())
            assertTrue(session.scope(text, SlotKind.LABEL).isEmpty())
            session.locate(Position(1, 2))
            assertEquals(listOf(NodeKind.PARAGRAPH), session.context.blocks.map { it.kind })
            val final = session.finalize()
            val link = assertIs<Link>(assertIs<Paragraph>(final.content.first()).content.first())
            assertTrue(session.markers(link).isNotEmpty())
        }
    }

    @Test
    fun theLexiconsAreTheDialects() {
        assertEquals(
            listOf("name", "title", "subtitle", "time", "date", "authors", "keywords", "abstract", "state", "comment"),
            Metadata.fields,
        )
        assertTrue('[' in Slot.cues && '\n' in Slot.cues && '>' !in Slot.cues)
        assertEquals(Slot.cues.sorted(), Slot.cues)
        assertEquals("CrossLink", NodeKind.CROSS_LINK.toString())
        assertEquals("class", SlotKind.CLASS.toString())
    }
}
