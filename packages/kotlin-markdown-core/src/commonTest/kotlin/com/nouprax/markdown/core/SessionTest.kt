package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertSame
import kotlin.test.assertTrue

/** A session's document is the document of its text, whatever steps led there. */
private fun assertParsesItsText(session: MarkdownSession) {
    val text = session.text
    assertEquals(Document.parse(text).dump(text), session.document.dump(text))
}

/** The text every edit sweep starts from: ASCII, so a column is a byte. */
private const val SWEEP = "# Title [a]\n\nfirst *one* `code`\n\n- item\n- second\n\n> quote\n\n[a]: /url\n"

/** What each sweep writes at a cursor, and so removes again. */
private val writes = listOf("*", "\n", "- ", "#", "`", "> ", "[a]", "x")

/** Every cursor of an ASCII [text]: each line's columns, its end included. */
private fun cursors(text: String): kotlin.collections.List<Position> =
    text.split("\n").flatMapIndexed { line, content -> (1..content.length + 1).map { Position(line + 1, it) } }

/** The cursor after the ASCII [written] is written at [cursor]. */
private fun after(
    written: String,
    cursor: Position,
): Position {
    val lines = written.split("\n")
    if (lines.size == 1) return Position(cursor.line, cursor.column + written.length)
    return Position(cursor.line + lines.size - 1, lines.last().length + 1)
}

class SessionTest {
    @Test
    fun aNewSessionIsAParseOfItsSource() {
        val source = "# Title\n\n[^n]: note\n\nSome *text*[^n]\n"
        MarkdownSession(source).use { session ->
            assertEquals(TextUnit.UTF16, session.unit)
            assertEquals(TextUnit.UTF16, session.document.unit)
            assertEquals(source, session.text)
            // Identifiers included: a session numbers its first document as a parse does.
            assertEquals(Document.parse(source), session.document)
        }
        MarkdownSession().use { session ->
            assertEquals("", session.text)
            assertEquals(Document.parse(""), session.document)
        }
    }

    @Test
    fun anAppendKeepsTheIdentifiersOfTheNodesItContinues() {
        MarkdownSession("# Title\n\nfirst\n\nsecond").use { session ->
            val before = session.document
            val after = session.append(" paragraph\n\n> quote\n")
            assertEquals("# Title\n\nfirst\n\nsecond paragraph\n\n> quote\n", session.text)
            assertEquals(after, session.document)
            assertParsesItsText(session)
            // The heading and the first paragraph are unchanged values.
            assertEquals(before.content[0], after.content[0])
            assertEquals(before.content[1], after.content[1])
            // The extended paragraph continues the old one under its identifier.
            val extended = assertIs<Paragraph>(after.content[2])
            assertEquals(before.content[2].id, extended.id)
            assertNotEquals(before.content[2], extended)
            assertEquals("second paragraph", assertIs<Text>(extended.content.single()).literal)
            assertIs<Callout>(after.content[3])
        }
    }

    @Test
    fun theDocumentsOfASessionCountColumnsInItsUnit() {
        // 🚀 is two UTF-16 units and four UTF-8 bytes.
        for ((unit, end) in listOf(TextUnit.UTF16 to 4, TextUnit.UTF8 to 6)) {
            MarkdownSession("🚀a", unit).use { session ->
                val document = session.append("b\n")
                assertEquals(unit, document.unit)
                assertParsesItsText(session)
                val text = assertIs<Paragraph>(document.content.single()).content.single()
                assertEquals(listOf(Scope(Position(1, 1), Position(1, end))), document.scope(text, session.text))
            }
        }
    }

    @Test
    fun flushReturnsTheFinalDocumentAndEndsTheSession() {
        for (unit in listOf(TextUnit.UTF8, TextUnit.UTF16)) {
            MarkdownSession("[a]\n\n", unit).use { session ->
                val snapshot = session.append("[a]: /url\n")
                // A snapshot's lookup sees only the definitions written before it.
                assertNotEquals(Document.parse(session.text, unit).dump(session.text), snapshot.dump(session.text))
                val final = session.flush()
                val text = session.text
                assertEquals(final, session.document)
                assertEquals(Document.parse(text, unit).dump(text), final.dump(text))
                assertEquals(
                    ErrorCode.SESSION_FLUSHED,
                    assertFailsWith<MarkdownCoreException> { session.append("more") }.code,
                )
                assertEquals(
                    ErrorCode.SESSION_FLUSHED,
                    assertFailsWith<MarkdownCoreException> { session.flush() }.code,
                )
                assertEquals(text, session.text)
                assertEquals(final, session.document)
            }
        }
    }

    @Test
    fun anEditAtEachCursorAndItsUndoParseAsTheNewText() {
        for (unit in listOf(TextUnit.UTF8, TextUnit.UTF16)) {
            MarkdownSession(SWEEP, unit).use { session ->
                session.flush()
                cursors(SWEEP).forEachIndexed { index, place ->
                    val written = writes[index % writes.size]
                    val document = session.edit(place, place, written)
                    assertEquals(document, session.document)
                    val text = session.text
                    assertEquals(Document.parse(text, unit).dump(text), document.dump(text), "$written at $place")
                    session.edit(place, after(written, place), "")
                    assertEquals(SWEEP, session.text)
                    val undone = session.document.dump(SWEEP)
                    assertEquals(Document.parse(SWEEP, unit).dump(SWEEP), undone, "$written undone at $place")
                }
            }
        }
    }

    @Test
    fun anEditsCursorsCountColumnsInTheSessionsUnit() {
        // 🚀 is four UTF-8 bytes and two UTF-16 units.
        for ((unit, column) in listOf(TextUnit.UTF8 to 5, TextUnit.UTF16 to 3)) {
            MarkdownSession("🚀 one\n\ntwo\n", unit).use { session ->
                session.flush()
                session.edit(Position(1, column), Position(1, column + 4), "*won*")
                assertEquals("🚀*won*\n\ntwo\n", session.text)
                assertEquals(Document.parse(session.text, unit).dump(session.text), session.document.dump(session.text))
            }
        }
    }

    @Test
    fun aNodeTheEditDidNotReachIsTheOldNodeAndOneItReadAgainKeepsItsIdentifier() {
        MarkdownSession("# Title\n\nfirst\n\nsecond\n").use { session ->
            val before = session.flush()
            val after = session.edit(Position(5, 7), Position(5, 7), " one")
            assertEquals("# Title\n\nfirst\n\nsecond one\n", session.text)
            assertEquals(before.content[0], after.content[0])
            assertEquals(before.content[1], after.content[1])
            assertEquals(before.content[2].id, after.content[2].id)
            assertNotEquals(before.content[2], after.content[2])
        }
    }

    @Test
    fun anEditBeforeFlushOutOfBoundsOrBackwardsThrowsAndLeavesTheSessionAlone() {
        MarkdownSession("- text\n").use { session ->
            val start = Position(1, 1)
            val inside = Position(1, 4)
            val nowhere = Position(9, 1)

            fun refused(
                from: Position,
                to: Position,
            ) = assertFailsWith<MarkdownCoreException> { session.edit(from, to, "") }.code

            assertEquals(ErrorCode.SESSION_STREAMING, refused(start, start))
            session.append("more\n")
            val final = session.flush()
            session.locate(inside)
            val open = session.context
            assertTrue(open.blocks.isNotEmpty())
            assertEquals(ErrorCode.OUT_OF_BOUNDS, refused(inside, start))
            assertEquals(ErrorCode.OUT_OF_BOUNDS, refused(nowhere, nowhere))
            assertEquals(ErrorCode.OUT_OF_BOUNDS, refused(Position(0, 1), start))
            assertEquals("- text\nmore\n", session.text)
            assertEquals(final, session.document)
            assertSame(open, session.context)
            // An edit that runs empties the context, since it changes the text.
            session.edit(start, start, "a ")
            assertTrue(session.context.blocks.isEmpty())
        }
    }

    @Test
    fun aClosedSessionTakesNoFurtherCall() {
        val session = MarkdownSession("text\n")
        session.close()
        session.close()
        assertFailsWith<IllegalStateException> { session.append("more") }
        assertFailsWith<IllegalStateException> { session.flush() }
        assertFailsWith<IllegalStateException> { session.edit(Position(1, 1), Position(1, 1), "") }
        assertFailsWith<IllegalStateException> { session.text }
        assertEquals(1, session.document.content.size)
    }
}
