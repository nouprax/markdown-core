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
 * The source a scope's coordinates are counted against, copied into value types
 * and read after the native handle is gone. `parse` frees the handle before it
 * returns, so everything below reads a value with no native anything behind it.
 */
class ConcreteTest {
    @Test
    fun theSourceAndItsLineIndexSurviveTheNativeRelease() {
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

        // Every line but the first begins after a line ending.
        for (line in 2..concrete.lineCount) {
            val start = concrete.lineStart(line)
            assertTrue(start > 0)
            assertEquals('\n'.code.toByte(), concrete.source[start - 1])
        }

        // Nothing native is left: 300 more parses cannot move what was copied.
        repeat(300) { Document.parse("# copy\n") }
        assertContentEquals(source.encodeToByteArray(), concrete.source)
        assertEquals(14, concrete.lineStart(3))
    }
}
