package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * The stream (docs/STREAMING.md §4 D5). Everything here reads a [Document]
 * value the session returned, and nothing native stands behind it: a feed's
 * answer outlives every later feed, the finish, and the session itself.
 */
class SessionTest {
    // CRLF line endings and characters of two, three and four UTF-8 bytes, so
    // that a byte-level chunk boundary can land inside a line ending and
    // inside every multi-byte width there is.
    private val source =
        listOf(
            "# Héllo 🚀 中文",
            "",
            "> quoted *em* and `code`",
            "",
            "- [x] tick",
            "- plain",
            "",
            "| a | b |",
            "| --- | :-: |",
            "| c | d |",
            "",
            "``` kotlin",
            "val rocket = \"🚀\"",
            "```",
            "",
            "see [a] and \$x\$.",
            "",
            "[a]: /url \"t\"",
            "",
        ).joinToString("\r\n")

    @Test
    fun chunkedFeedThenFinishEqualsTheOneShotParse() {
        // Seven is prime, so the boundaries drift: chunks split the CRLF
        // pairs, the emoji, and the CJK characters mid-sequence, and the
        // sealed document must not be able to tell.
        val bytes = source.encodeToByteArray()
        Session().use { session ->
            var index = 0
            while (index < bytes.size) {
                val end = minOf(index + 7, bytes.size)
                session.feed(bytes.copyOfRange(index, end))
                index = end
            }
            val sealed = session.finish()
            val oneShot = Document.parse(source)
            assertEquals(oneShot.dump(), sealed.dump())
            assertEquals(oneShot.scope, sealed.scope)
            assertContentEquals(oneShot.concrete.source, sealed.concrete.source)
            assertEquals(oneShot.concrete.lineCount, sealed.concrete.lineCount)
            for (line in 1..oneShot.concrete.lineCount) {
                assertEquals(oneShot.concrete.lineStart(line), sealed.concrete.lineStart(line))
            }
        }
    }

    @Test
    fun aSessionReadsTheSameOptionsTheOneShotParseDoes() {
        val markdown = "| a |\n| --- |\n| b |\n"
        Session(ParseOptions(tables = false)).use { session ->
            session.feed(markdown)
            assertIs<Paragraph>(session.finish().content.first())
        }
        Session().use { session ->
            session.feed(markdown)
            assertIs<Table>(session.finish().content.first())
        }
    }

    @Test
    fun aMidStreamDocumentIsUsableAndUnchangedByLaterFeeds() {
        val session = Session()
        // The trailing line's ending has not arrived, so "two" is not yet in
        // the projection -- not in the tree and not in the concrete view.
        val first = session.feed("# One\n\ntwo")
        assertIs<Heading>(first.content.single())
        assertEquals("# One\n\n", first.concrete.source.decodeToString())
        val before = first.dump()

        // A later feed completes the line; the value already returned does
        // not move, and the new answer carries the completed line.
        val second = session.feed(" plus\nthree\n")
        assertEquals(before, first.dump())
        assertIs<Heading>(first.content.single())
        val paragraph = assertIs<Paragraph>(second.content[1])
        assertEquals("two plus", assertIs<Text>(paragraph.content.first()).literal)

        // The sealed document equals the one-shot parse of the same bytes,
        // and every earlier answer survives the session's death.
        val sealed = session.finish()
        session.close()
        assertEquals(Document.parse("# One\n\ntwo plus\nthree\n").dump(), sealed.dump())
        assertEquals(before, first.dump())
    }

    @Test
    fun anEmptyFeedIsLegalAndFinishingAnUnfedSessionEqualsParsingNothing() {
        Session().use { session ->
            assertTrue(session.feed(ByteArray(0)).content.isEmpty())
            assertTrue(session.feed("").content.isEmpty())
            val sealed = session.finish()
            assertTrue(sealed.content.isEmpty())
            assertEquals(Document.parse("").dump(), sealed.dump())
        }
    }

    @Test
    fun feedAfterFinishReportsInvalidArgument() {
        val session = Session()
        session.feed("# a\n")
        session.finish()
        // The stream is sealed: the refusal is the parser's, crosses the wire
        // as an error, and names the code the C surface rules for it.
        assertEquals(
            ParseErrorCode.INVALID_ARGUMENT,
            assertFailsWith<ParseException> { session.feed("more") }.code,
        )
        assertEquals(
            ParseErrorCode.INVALID_ARGUMENT,
            assertFailsWith<ParseException> { session.finish() }.code,
        )
        // Only close remains, and it still works.
        session.close()
    }

    @Test
    fun aClosedSessionRefusesEveryCallAndCloseIsIdempotent() {
        val session = Session()
        session.close()
        session.close()
        assertFailsWith<IllegalStateException> { session.feed("x") }
        assertFailsWith<IllegalStateException> { session.finish() }
    }
}
