package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * The living document (docs/STREAMING.md §4 D5, under 3.0's names).
 * Everything here reads a [Read] value the document returned, and nothing
 * native stands behind it: a feed's answer outlives every later feed, the
 * seal, and the document itself.
 */
class DocumentTest {
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
    fun chunkedFeedThenSealEqualsTheWholeTextParse() {
        // Seven is prime, so the boundaries drift: chunks split the CRLF
        // pairs, the emoji, and the CJK characters mid-sequence, and the
        // sealed read must not be able to tell.
        val bytes = source.encodeToByteArray()
        val document = Document()
        var index = 0
        while (index < bytes.size) {
            val end = minOf(index + 7, bytes.size)
            document.feed(bytes.copyOfRange(index, end))
            index = end
        }
        val sealed = document.seal()
        val wholeText = Document(source).seal()
        assertEquals(wholeText.dump(), sealed.dump())
        assertEquals(wholeText.semantic.scope, sealed.semantic.scope)
        assertContentEquals(wholeText.concrete.source, sealed.concrete.source)
        assertEquals(wholeText.concrete.lines, sealed.concrete.lines)
        for (line in 1..wholeText.concrete.lines) {
            assertEquals(wholeText.concrete.offset(line), sealed.concrete.offset(line))
        }
    }

    @Test
    fun aDocumentReadsTheSameOptionsHoweverItWasOpened() {
        val markdown = "| a |\n| --- |\n| b |\n"
        assertIs<Paragraph>(
            Document(markdown, ParseOptions(tables = false))
                .seal()
                .semantic.content
                .first(),
        )
        Document(ParseOptions(tables = false)).use { document ->
            document.feed(markdown)
            assertIs<Paragraph>(
                document
                    .seal()
                    .semantic.content
                    .first(),
            )
        }
        Document().use { document ->
            document.feed(markdown)
            assertIs<Table>(
                document
                    .seal()
                    .semantic.content
                    .first(),
            )
        }
    }

    @Test
    fun aMidStreamReadIsUsableAndUnchangedByLaterFeeds() {
        val document = Document()
        // The trailing line's ending has not arrived, so "two" is not yet in
        // the projection -- not in the tree and not in the concrete view.
        val first = document.feed("# One\n\ntwo")
        assertIs<Heading>(first.semantic.content.single())
        assertEquals("# One\n\n", first.concrete.source.decodeToString())
        val before = first.dump()

        // A later feed completes the line; the value already returned does
        // not move, and the new answer carries the completed line.
        val second = document.feed(" plus\nthree\n")
        assertEquals(before, first.dump())
        assertIs<Heading>(first.semantic.content.single())
        val paragraph = assertIs<Paragraph>(second.semantic.content[1])
        assertEquals("two plus", assertIs<Text>(paragraph.content.first()).literal)

        // The sealed read equals the whole-text parse of the same bytes, and
        // every earlier answer survives the document's death.
        val sealed = document.seal()
        assertEquals(Document("# One\n\ntwo plus\nthree\n").seal().dump(), sealed.dump())
        assertEquals(before, first.dump())
    }

    @Test
    fun anEmptyFeedIsLegalAndSealingAnUnfedDocumentEqualsParsingNothing() {
        val document = Document()
        assertTrue(
            document
                .feed(ByteArray(0))
                .semantic.content
                .isEmpty(),
        )
        assertTrue(
            document
                .feed("")
                .semantic.content
                .isEmpty(),
        )
        val sealed = document.seal()
        assertTrue(sealed.semantic.content.isEmpty())
        assertEquals(Document("").seal().dump(), sealed.dump())
    }

    @Test
    fun sealReleasesTheShellAndASealedDocumentRefusesEveryCall() {
        val document = Document("# a\n")
        document.seal()
        // Sealing IS closing: nothing native remains, so a later call is a
        // use of a dead object rather than a parse error crossing the wire.
        assertFailsWith<IllegalStateException> { document.feed("more") }
        assertFailsWith<IllegalStateException> { document.seal() }
        // Close remains legal and idempotent on what is already gone.
        document.close()
        document.close()
    }

    @Test
    fun aBlockKeepsItsIdentityAcrossFeedsAndAReferenceNamesTheFirstDefinition() {
        Document().use { document ->
            // The heading is the element a consumer renders; later feeds and
            // the seal must keep calling it by the same name (D4).
            val first = document.feed("# Title\n\nsee [a] and [^n].\n\n")
            val heading = first.semantic.content.first()
            assertIs<Heading>(heading)
            val second = document.feed("[a]: /first\n\n[a]: /second\n\n[^n]: note\n")
            assertEquals(
                heading.id,
                second.semantic.content
                    .first()
                    .id,
            )
            val sealed = document.seal()
            assertEquals(
                heading.id,
                sealed.semantic.content
                    .first()
                    .id,
            )

            // Duplicate definitions: both stay in the tree, and the reference
            // names the FIRST by identity -- its own match key is the winning
            // definition's norm.
            val definitions = sealed.semantic.content.filterIsInstance<ReferenceDefinition>()
            assertEquals(2, definitions.size)
            val paragraph = assertIs<Paragraph>(sealed.semantic.content[1])
            val reference = paragraph.content.filterIsInstance<LinkReference>().single()
            assertEquals(definitions[0].id, reference.definition)
            assertEquals("a", definitions[0].norm)
            val footnote =
                sealed.semantic.content
                    .filterIsInstance<FootnoteDefinition>()
                    .single()
            assertEquals(
                footnote.id,
                paragraph.content
                    .filterIsInstance<FootnoteReference>()
                    .single()
                    .definition,
            )
            assertEquals("^n", footnote.norm)

            // An inline's identity is (owning block, ordinal): unique within
            // its paragraph, owned by it.
            paragraph.content.forEach { assertEquals(paragraph.id.block, it.id.block) }
            assertEquals(
                paragraph.content.size,
                paragraph.content
                    .map { it.id }
                    .toSet()
                    .size,
            )
        }
    }

    @Test
    fun aClosedDocumentRefusesEveryCallAndCloseIsIdempotent() {
        val document = Document()
        document.close()
        document.close()
        assertFailsWith<IllegalStateException> { document.feed("x") }
        assertFailsWith<IllegalStateException> { document.seal() }
    }
}
