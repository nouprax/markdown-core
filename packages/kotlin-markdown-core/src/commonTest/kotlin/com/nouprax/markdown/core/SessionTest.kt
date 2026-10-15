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
    fun aClosedSessionTakesNoFurtherCall() {
        val session = MarkdownSession("text\n")
        session.close()
        session.close()
        assertFailsWith<IllegalStateException> { session.append("more") }
        assertFailsWith<IllegalStateException> { session.text }
        assertEquals(1, session.document.content.size)
    }
}
