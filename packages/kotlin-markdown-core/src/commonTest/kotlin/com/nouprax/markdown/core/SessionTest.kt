package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals

/** A session's document is the document of its text, whatever steps led there. */
private fun assertParsesItsText(session: MarkdownSession) {
    val text = session.text
    assertEquals(Document.parse(text).dump(text), session.document.dump(text))
}

class SessionTest {
    @Test
    fun aNewSessionIsAParseOfItsSource() {
        val source = "# Title\n\nSome *text*[^n]\n\n[^n]: note\n"
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
    fun anEditKeepsTheIdentifiersOfTheNodesItContinues() {
        MarkdownSession("# Title\n\nfirst paragraph\n\nsecond\n").use { session ->
            val before = session.document
            val after = session.edit(listOf(TextEdit(9, 14, "edited")))
            assertEquals("# Title\n\nedited paragraph\n\nsecond\n", session.text)
            assertEquals(after, session.document)
            assertParsesItsText(session)
            // The heading and the last paragraph are unchanged values.
            assertEquals(before.content[0], after.content[0])
            assertEquals(before.content[2], after.content[2])
            // The edited paragraph continues the old one under its identifier.
            val edited = assertIs<Paragraph>(after.content[1])
            assertEquals(before.content[1].id, edited.id)
            assertNotEquals(before.content[1], edited)
            assertEquals("edited paragraph", assertIs<Text>(edited.content.single()).literal)
        }
    }

    @Test
    fun aBatchAppliesItsEditsToTheTextBeforeItInAnyOrder() {
        MarkdownSession("a b c\n").use { session ->
            session.edit(listOf(TextEdit(4, 5, "C"), TextEdit(0, 1, "A"), TextEdit(2, 2, "x"), TextEdit(2, 2, "y")))
            // Two edits at one offset apply in the order listed.
            assertEquals("A xyb C\n", session.text)
            assertParsesItsText(session)
            session.edit(emptyList())
            assertEquals("A xyb C\n", session.text)
            assertParsesItsText(session)
        }
    }

    @Test
    fun appendIsTheEditAtTheEndOfTheText() {
        MarkdownSession("para").use { session ->
            val before = session.document
            session.append("graph\n\n> quote\n")
            assertEquals("paragraph\n\n> quote\n", session.text)
            assertParsesItsText(session)
            assertEquals(before.content.single().id, session.document.content[0].id)
            assertIs<Callout>(session.document.content[1])
        }
    }

    @Test
    fun offsetsCountInTheSessionUnit() {
        // 🚀 is two UTF-16 units and four UTF-8 bytes: read as bytes, offset 2
        // would fall inside it.
        MarkdownSession("🚀a\n").use { session ->
            session.edit(listOf(TextEdit(2, 3, "b")))
            assertEquals("🚀b\n", session.text)
            assertParsesItsText(session)
        }
        MarkdownSession("🚀a\n", TextUnit.UTF8).use { session ->
            assertEquals(TextUnit.UTF8, session.document.unit)
            session.edit(listOf(TextEdit(4, 5, "b")))
            assertEquals("🚀b\n", session.text)
            assertEquals(TextUnit.UTF8, session.document.unit)
        }
    }

    @Test
    fun spansTheSessionRejectsCarryTheirCode() {
        MarkdownSession("🚀 abc\n").use { session ->
            val rejected =
                listOf(
                    listOf(TextEdit(4, 3, "")) to ErrorCode.OUT_OF_BOUNDS,
                    listOf(TextEdit(0, 8, "")) to ErrorCode.OUT_OF_BOUNDS,
                    listOf(TextEdit(-1, 0, "")) to ErrorCode.OUT_OF_BOUNDS,
                    listOf(TextEdit(3, 5, ""), TextEdit(4, 6, "")) to ErrorCode.OUT_OF_BOUNDS,
                    listOf(TextEdit(1, 1, "x")) to ErrorCode.INSIDE_SCALAR,
                    listOf(TextEdit(0, 1, "")) to ErrorCode.INSIDE_SCALAR,
                )
            for ((edits, code) in rejected) {
                val failure = assertFailsWith<MarkdownCoreException> { session.edit(edits) }
                assertEquals(code, failure.code, edits.toString())
            }
        }
    }

    @Test
    fun aClosedSessionTakesNoFurtherCall() {
        val session = MarkdownSession("text\n")
        session.close()
        session.close()
        assertFailsWith<IllegalStateException> { session.edit(emptyList()) }
        assertFailsWith<IllegalStateException> { session.append("more") }
        assertFailsWith<IllegalStateException> { session.text }
        assertEquals(1, session.document.content.size)
    }
}
