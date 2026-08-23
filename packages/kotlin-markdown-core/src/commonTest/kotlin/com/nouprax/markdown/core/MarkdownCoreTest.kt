package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

class ApiTest {
    @Test
    fun defaultsAndOptionGates() {
        val defaults = ParseOptions()
        assertTrue(defaults.smartPunctuation && defaults.footnotes && defaults.stripHTMLComments)
        assertTrue(defaults.tables && defaults.strikethrough && defaults.autolinks)
        assertTrue(defaults.taskLists && defaults.formulas && defaults.directives)

        val markdown = "| a |\n| --- |\n| b |\n"
        assertIs<Table>(
            Document
                .parse(markdown)
                .content
                .first(),
        )
        assertIs<Paragraph>(
            Document
                .parse(markdown, ParseOptions(tables = false))
                .content
                .first(),
        )
    }

    @Test
    fun visitorAndWalkerAreTypedAndDepthFirst() {
        val document = Document.parse("# Heading\n\nBody\n")
        val visitor = KindVisitor()
        assertEquals("heading:1", document.content.first().accept(visitor))
        val recordingVisitor = RecordingVisitor()
        Walker.walk(document, recordingVisitor)
        assertEquals("Document", recordingVisitor.visited.first())
        assertTrue("Heading" in recordingVisitor.visited && "Text" in recordingVisitor.visited)
    }
}

class UnicodeTest {
    @Test
    fun standardUtf8SurvivesTheNativeBoundary() {
        val document = Document.parse("héllo 🚀 中文\n")
        val paragraph = assertIs<Paragraph>(document.content.first())
        assertEquals("héllo 🚀 中文", assertIs<Text>(paragraph.content.first()).literal)
    }
}

class ErrorsTest {
    @Test
    fun emptyInputIsAValidDocument() {
        assertTrue(
            Document
                .parse("")
                .content
                .isEmpty(),
        )
    }

    @Test
    fun corruptedNativePayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument(byteArrayOf(0x4d, 0x4b, 0x43))
        }
    }
}

class OwnershipTest {
    @Test
    fun returnedTreesOutliveEveryNativeDocument() {
        val documents = kotlin.collections.List(300) { Document.parse("# Copy\n\n- [x] item\n") }
        assertTrue(documents.all { it.content.size == 2 })
        assertEquals(1, assertIs<Heading>(documents.last().content.first()).level)
    }

    @Test
    fun readOnlyCollectionsDoNotLeakMutableImplementations() {
        val content = Document.parse("one *two* three\n").content
        assertFailsWith<ClassCastException> {
            @Suppress("UNCHECKED_CAST")
            (content as MutableList<Markup>).clear()
        }
    }
}

class RobustnessTest {
    @Test
    fun largeDocumentsCopyCompletelyBeforeNativeRelease() {
        val unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
        val document = Document.parse(unit.repeat(5_000))
        assertEquals(10_000, document.content.size)
    }

    @Test
    fun deepBlockQuoteNestingRemainsTraversable() {
        val depth = 128
        var node: Markup =
            Document
                .parse("> ".repeat(depth) + "leaf\n")
                .content
                .single()
        repeat(depth) {
            val quote = assertIs<BlockQuote>(node)
            node = quote.content.first()
        }
        assertIs<Paragraph>(node)
    }

    @Test
    fun repeatedParseAndReleaseRemainsStable() {
        repeat(2_000) {
            assertEquals(
                2,
                Document
                    .parse("# Copy\n\n- [x] item 🚀\n")
                    .content.size,
            )
        }
    }
}

/**
 * The requirement's own sentence: the concrete view survives being copied into
 * value types and the handle being freed. `parse` frees it before it returns,
 * so everything below reads a view with no native anything left behind it.
 */
class ConcreteTest {
    @Test
    fun theViewIsTotalAndItsOwnersResolveAfterNativeRelease() {
        val source =
            listOf(
                "# Heading ##",
                "",
                "> quoted *em* and `code`",
                "",
                "| a | b |",
                "| --- | --- |",
                "| c | d |",
                "",
                ":::container[Title]{kind=demo}",
                "Body",
                ":::",
                "",
                """[a]: /u "t"""",
                "",
                "see [a].",
                "",
            ).joinToString("\n")
        val document = Document.parse(source)
        val concrete = document.concrete
        assertContentEquals(source.encodeToByteArray(), concrete.source)
        assertEquals(15, concrete.lineCount)
        assertEquals(0, concrete.lineStart(1))
        assertEquals(14, concrete.lineStart(3))
        assertFailsWith<IndexOutOfBoundsException> { concrete.lineStart(0) }
        assertFailsWith<IndexOutOfBoundsException> { concrete.lineStart(16) }
        assertFailsWith<IndexOutOfBoundsException> { concrete.region(concrete.regionCount) }

        var covered = 0
        var markers = 0
        repeat(concrete.regionCount) { index ->
            val region = concrete.region(index)
            assertEquals(covered, region.start)
            assertTrue(region.length > 0)
            covered += region.length
            assertTrue(document.ownerOf(region) != null)
            if (region.role == RegionRole.MARKER) markers += 1
        }
        // The heading's closing `##`, the table's pipes and the definition's
        // punctuation are in no literal anywhere in the semantic tree, and the
        // line above says every byte of them is in a region here.
        assertEquals(concrete.source.size, covered)
        assertTrue(markers > 0)
        assertEquals(null, document.ownerOf(Region(0, 1, RegionRole.CONTENT, listOf(99))))

        // THE DESCENT IS THE C CHILD ORDER, not the value tree's named fields.
        // A table holds its header BEFORE its rows, so byte 42 -- the `a` of
        // the header row -- has to land on line 5 and not on line 7; a
        // directive holds its LABEL before its content, so byte 106 -- the `B`
        // of `Body` -- has to land on line 10 and not inside the label on 9.
        assertEquals(Position(5, 3), document.ownerAt(42).scope.start)
        assertEquals(Position(10, 1), document.ownerAt(106).scope.start)

        // Nothing native is left: 300 more parses cannot move what was copied.
        repeat(300) { Document.parse("# copy\n") }
        assertContentEquals(source.encodeToByteArray(), concrete.source)
        assertEquals(0, concrete.region(0).start)
    }

    /** The owner of the region the byte at [offset] belongs to. */
    private fun Document.ownerAt(offset: Int): Markup {
        repeat(concrete.regionCount) { index ->
            val region = concrete.region(index)
            if (offset >= region.start && offset < region.start + region.length) {
                return requireNotNull(ownerOf(region))
            }
        }
        error("no region holds byte $offset")
    }
}
