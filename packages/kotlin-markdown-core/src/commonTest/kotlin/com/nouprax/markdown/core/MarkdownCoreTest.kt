package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
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

class IdentityTest {
    @Test
    fun equalityIsSeriesSaltedIdentityPlusRevision() {
        Document("Same *content* twice.\n").use { first ->
            Document("Same *content* twice.\n").use { second ->
                // Identical content from different parses never compares equal.
                assertNotEquals<Markup>(first, second)
                assertNotEquals<Markup>(first.content[0], second.content[0])
                // Within one document, identity is value equality.
                assertEquals(first.content[0], first.content[0])
                assertNotEquals(first.id.series, second.id.series)
                // An id from another series is not this document's to answer.
                assertNull(first.node(second.content[0].id))
            }
        }
    }

    @Test
    fun documentsAreValuesAndIdsAreStableMapKeys() {
        Document("").use { empty ->
            assertTrue(empty.content.isEmpty())
            empty.append("Alpha\n").use { document ->
                val paragraph = assertIs<Paragraph>(document.content[0])
                val byId = hashMapOf(paragraph.id to "paragraph")
                assertEquals(paragraph.id, document.node(paragraph.id)?.id)
                // The root answers for itself, which is what a consumer
                // reconciling by id needs when the top-level list changes.
                assertEquals(document.id, document.node(document.id)?.id)
                assertEquals("paragraph", byId[paragraph.id])
                assertNull(document.node(MarkupID(document.series + 1UL, paragraph.id.rawValue)))
            }
        }
    }

    @Test
    fun diagnosticsTravelWithTheDocumentThatRaisedThem() {
        // The one thing an editor underlines: a directive's `{...}` did not
        // parse, so the braces stayed literal text. Invisible in the tree —
        // the node simply has no attributes — which is why it is reported.
        val before = Document(":::note{= bad}\nbody\n:::\n")
        assertEquals(listOf(DiagnosticCode.DIRECTIVE_ATTRIBUTES), before.diagnostics.map { it.code })
        assertEquals(Scope(Position(1, 8), Position(1, 14)), before.diagnostics.single().scope)

        // A well-formed directive raises nothing...
        Document(":::note{a=1}\nbody\n:::\n").use { clean ->
            assertTrue(clean.diagnostics.isEmpty())
        }
        // ...and appending carries the raiser's diagnostic forward, because
        // the text that raised it is still in the document.
        before.append("\ntail\n").use { after ->
            assertEquals(1, after.diagnostics.size)
        }
        // The predecessor keeps reporting its own: diagnostics are values it
        // copied out at parse time, like every other part of it.
        assertEquals(1, before.diagnostics.size)
        before.close()
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
            decodeWire(byteArrayOf(0x4d, 0x4b, 0x43)) { _, _, _, _, _, _, _ ->
                error("a truncated payload reached the build step")
            }
        }
    }

    @Test
    fun aDecodeFailurePastTheHandleReleasesTheParseItCouldNotDeliver() {
        // A REAL parse whose payload is then cut off right behind the handle
        // — magic(4) + status(1) + handle(8) — so the decoder holds a live
        // native document and the very next read throws. The release this
        // forces runs against a handle the engine really issued; freeing a
        // wrong or already-freed one would take the suite down natively.
        val payload = cOpen("# leak window\n".encodeToByteArray(), ParseOptions())
        assertFailsWith<IllegalArgumentException> {
            decodeWire(payload.copyOf(13)) { _, _, _, _, _, _, _ ->
                error("a truncated payload reached the build step")
            }
        }
    }

    @Test
    fun aThrowingBuildStepReleasesTheParseAndRethrowsTheCallersFailure() {
        // The other half of the window: the whole payload decodes and the
        // build step itself throws. The parse is released and the caller's
        // own exception surfaces unwrapped.
        val payload = cOpen("# leak window\n".encodeToByteArray(), ParseOptions())
        val failure =
            assertFailsWith<IllegalStateException> {
                decodeWire(payload) { _, _, _, _, _, _, _ ->
                    error("the build step failed after a complete decode")
                }
            }
        assertEquals("the build step failed after a complete decode", failure.message)
    }

    @Test
    fun aCraftedPayloadWithoutAParseFreesNothing() {
        // Zero is never a valid handle: a hand-built payload that carries
        // none must fail its decode without a release call reaching the
        // native side.
        val zeroHandle = byteArrayOf(0x4d, 0x4b, 0x43, 0x35, 0x00, 0, 0, 0, 0, 0, 0, 0, 0)
        assertFailsWith<IllegalArgumentException> {
            decodeWire(zeroHandle) { _, _, _, _, _, _, _ ->
                error("a truncated payload reached the build step")
            }
        }
    }

    @Test
    fun aDeliveryLostStatusSurfacesThroughItsHandler() {
        // Status 2: the native mutation succeeded but its payload was lost —
        // the wire's whole answer is the one byte, routed to the caller's
        // handler; without one it is an unsupported status.
        assertFailsWith<IllegalStateException> {
            decodeWire(
                byteArrayOf(0x4d, 0x4b, 0x43, 0x35, 0x02),
                onDeliveryLost = { throw IllegalStateException("the chain is done") },
            ) { _, _, _, _, _, _, _ ->
                error("a delivery-lost payload reached the build step")
            }
        }
        assertFailsWith<IllegalStateException> {
            decodeWire(byteArrayOf(0x4d, 0x4b, 0x43, 0x35, 0x02)) { _, _, _, _, _, _, _ ->
                error("a delivery-lost payload reached the build step")
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
