package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

class ApiTest {
    @Test
    fun defaultsAndOptionGates() {
        val defaults = ParseOptions()
        assertTrue(defaults.smartPunctuation && defaults.footnotes)
        assertTrue(defaults.tables && defaults.strikethrough && defaults.autolinks)
        assertTrue(defaults.taskLists && defaults.formulas && defaults.directives)
        assertTrue(defaults.crossLinks && defaults.embeds)

        val markdown = "| a |\n| --- |\n| b |\n"
        assertIs<Table>(Document(markdown).content.first())
        assertIs<Paragraph>(Document(markdown, ParseOptions(tables = false)).content.first())
    }

    @Test
    fun crossReferencesAreTypedSourceFaithfulAndIndependentlyGated() {
        val source =
            "before [[folder/note#^block|display]] and ![[folder/note#^block|display]] after\n"
        val paragraph = assertIs<Paragraph>(Document(source).content.first())
        val crossLink = assertIs<CrossLink>(paragraph.content[1])
        val embed = assertIs<Embed>(paragraph.content[3])
        assertEquals("folder/note#^block|display", crossLink.reference)
        assertEquals("folder/note#^block|display", embed.reference)

        val linksDisabled =
            assertIs<Paragraph>(
                Document(source, ParseOptions(crossLinks = false)).content.first(),
            )
        assertIs<Embed>(linksDisabled.content[1])

        val embedsDisabled =
            assertIs<Paragraph>(
                Document(source, ParseOptions(embeds = false)).content.first(),
            )
        assertIs<CrossLink>(embedsDisabled.content[1])
    }

    @Test
    fun formulasOptionGatesEverySupportedSyntax() {
        val inlineDollar = Document("\$x\$\n")
        val blockDollar = Document("\$\$x\$\$\n")
        val inlineLaTeX = Document("\\\\(x\\\\)\n")
        val blockLaTeX = Document("\\\\[x\\\\]\n")
        val fenced = Document("```formula\nx\n```\n")

        assertIs<Formula>((inlineDollar.content.first() as Paragraph).content.first())
        assertIs<FormulaBlock>(blockDollar.content.first())
        assertIs<Formula>((inlineLaTeX.content.first() as Paragraph).content.first())
        assertIs<FormulaBlock>(blockLaTeX.content.first())
        assertIs<FormulaBlock>(fenced.content.first())

        val disabled = ParseOptions(formulas = false)
        assertIs<Text>((Document("\$x\$\n", disabled).content.first() as Paragraph).content.first())
        assertIs<Paragraph>(Document("\$\$x\$\$\n", disabled).content.first())
        assertIs<Text>(
            (Document("\\\\(x\\\\)\n", disabled).content.first() as Paragraph).content.first(),
        )
        assertIs<Paragraph>(Document("\\\\[x\\\\]\n", disabled).content.first())
        assertIs<CodeBlock>(Document("```formula\nx\n```\n", disabled).content.first())
    }

    @Test
    fun visitorAndWalkerAreTypedAndDepthFirst() {
        val document = Document("# Heading\n\nBody\n")
        val visitor = KindVisitor()
        assertEquals("heading:1", document.content.first().accept(visitor))
        val recordingVisitor = RecordingVisitor()
        MarkupWalker.walk(document, recordingVisitor)
        assertEquals("Document", recordingVisitor.visited.first())
        assertTrue("Heading" in recordingVisitor.visited && "Text" in recordingVisitor.visited)
    }
}

class UnicodeTest {
    @Test
    fun standardUtf8SurvivesTheNativeBoundary() {
        val document = Document("héllo 🚀 中文\n")
        val paragraph = assertIs<Paragraph>(document.content.first())
        assertEquals("héllo 🚀 中文", assertIs<Text>(paragraph.content.first()).literal)
    }
}

class ErrorsTest {
    @Test
    fun emptyInputIsAValidDocument() {
        assertTrue(Document("").content.isEmpty())
    }

    @Test
    fun corruptedNativePayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            decodeWireOpen(byteArrayOf(0x4d, 0x4b, 0x43)) { _, _, _, _, _, _, _ ->
                error("a truncated payload reached the build step")
            }
        }
    }
}

class OwnershipTest {
    @Test
    fun returnedTreesOutliveEveryNativeDocument() {
        val documents = kotlin.collections.List(300) { Document("# Copy\n\n- [x] item\n") }
        assertTrue(documents.all { it.content.size == 2 })
        assertEquals(1, assertIs<Heading>(documents.last().content.first()).level)
    }

    @Test
    fun readOnlyCollectionsDoNotLeakMutableImplementations() {
        val content = Document("one *two* three\n").content
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
        val document = Document(unit.repeat(5_000))
        assertEquals(10_000, document.content.size)
    }

    @Test
    fun deepBlockQuoteNestingRemainsTraversable() {
        val depth = 128
        var node: Markup = Document("> ".repeat(depth) + "leaf\n").content.single()
        repeat(depth) {
            val quote = assertIs<BlockQuote>(node)
            node = quote.content.first()
        }
        assertIs<Paragraph>(node)
    }

    @Test
    fun repeatedParseAndReleaseRemainsStable() {
        repeat(2_000) {
            assertEquals(2, Document("# Copy\n\n- [x] item 🚀\n").content.size)
        }
    }
}
