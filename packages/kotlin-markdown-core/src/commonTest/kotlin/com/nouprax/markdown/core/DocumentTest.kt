package com.nouprax.markdown.core

import kotlin.test.Test
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
            "> a quote",
            "> of two lines",
            "",
            ":::note[Title]{kind=demo}",
            "Body",
            ":::",
            "",
            ":::bare",
            "Body",
            ":::",
            "",
            "[^n]: a note",
            "    of two lines",
            "",
            "see [^n] too",
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
        // the projection.
        val first = document.feed("# One\n\ntwo")
        assertIs<Heading>(first.semantic.content.single())
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
    fun aFeedReusesThePreviousReadsValuesForEveryBlockThatDidNotMove() {
        // THE DELTA (#162): a feed's payload names, by position, the previous
        // read's children that the engine retained, and the decoder hands
        // those very values into the new read -- object identity, not a copy
        // -- so a consumer keyed on the previous read finds the same objects.
        // What changed is new: the open paragraph is re-read each time it
        // grows.
        Document().use { document ->
            val first = document.feed("# One\n\npara\n\ntail")
            assertEquals(2, first.semantic.content.size)
            val second = document.feed(" grows\n\n- item\n")
            assertEquals(4, second.semantic.content.size)
            assertTrue(second.semantic.content[0] === first.semantic.content[0])
            assertTrue(second.semantic.content[1] === first.semantic.content[1])
            assertIs<Paragraph>(second.semantic.content[2])
            assertIs<List>(second.semantic.content[3])
            // The seal is a delta too: everything closed before it is the
            // same value it was, and the read equals the whole-text parse.
            val sealed = document.seal()
            assertTrue(sealed.semantic.content[0] === second.semantic.content[0])
            assertTrue(sealed.semantic.content[1] === second.semantic.content[1])
            assertTrue(sealed.semantic.content[2] === second.semantic.content[2])
            assertEquals(Document("# One\n\npara\n\ntail grows\n\n- item\n").seal().dump(), sealed.dump())
            // A reused value is still a value: the earlier reads are unchanged.
            assertEquals(2, first.semantic.content.size)
            assertEquals(4, second.semantic.content.size)
        }
    }

    @Test
    fun deltaStreamedReadsEqualTheWholeReadsAtEveryLine() {
        // THE DELTA'S GATE (#162), over this suite's own source: a document
        // fed one line at a time answers every feed through a DELTA against
        // its previous read, and each such read must dump exactly as the read
        // a fresh document answers for the same bytes in one WHOLE frame; the
        // seal, a delta too, must equal the whole-text parse.
        val bytes = source.encodeToByteArray()
        Document().use { document ->
            var fed = 0
            var boundaries = 0
            while (fed < bytes.size) {
                var end = fed
                while (end < bytes.size && bytes[end] != '\n'.code.toByte()) end++
                if (end < bytes.size) end++
                val read = document.feed(bytes.copyOfRange(fed, end))
                fed = end
                boundaries++
                Document().use { whole ->
                    assertEquals(whole.feed(bytes.copyOfRange(0, fed)).dump(), read.dump(), "boundary $boundaries")
                }
            }
            assertTrue(boundaries > 20)
            assertEquals(Document(source).seal().dump(), document.seal().dump())
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
