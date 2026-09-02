package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

private fun wirePayload(vararg parts: Any): ByteArray {
    val out = mutableListOf<Byte>()
    for (part in parts) {
        when (part) {
            is String -> out += part.encodeToByteArray().toList()
            is Byte -> out += part
            is Int -> repeat(4) { shift -> out += ((part shr (shift * 8)) and 0xff).toByte() }
            else -> error("unsupported payload part")
        }
    }
    return out.toByteArray()
}

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

    @Test
    fun malformedWireValuesAreRejectedBeforeTheyEnterTheAst() {
        // The two sides of the wire are versioned separately -- MKC6 is the
        // current layout -- and a decoder that mapped an unknown value
        // instead of refusing it turns a protocol mismatch into a wrong
        // document. Nothing proved any of these fired.
        assertFailsWith<IllegalStateException> { WireKind.from(0) }
        assertFailsWith<IllegalStateException> { WireKind.from(33) }
        assertEquals(WireKind.IMAGE_REFERENCE, WireKind.from(32))

        // A header the decoder accepts, followed by nothing it can read.
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument("MKC6".encodeToByteArray())
        }
    }

    @Test
    fun malformedAndFailedWirePayloadsAreRejected() {
        // A native error crosses as a code and a message, which is the only
        // path that builds a ParseException.
        val failure =
            assertFailsWith<ParseException> {
                WireDecoder.decodeDocument(wirePayload("MKC6", 1.toByte(), 1, 3, "bad"))
            }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure.code)
        assertEquals("bad", failure.message)
        assertEquals(
            ParseErrorCode.INTERNAL,
            assertFailsWith<ParseException> {
                WireDecoder.decodeDocument(wirePayload("MKC6", 1.toByte(), 99, 1, "x"))
            }.code,
        )
        val allocationFailure =
            assertFailsWith<ParseException> {
                WireDecoder.decodeDocument(wirePayload("MKC6", 1.toByte(), 2, 13, "out of memory"))
            }
        assertEquals(ParseErrorCode.ALLOCATION_FAILED, allocationFailure.code)
        assertEquals("out of memory", allocationFailure.message)

        // A status that is neither, a magic from the wrong wire version, a
        // root that is not a document, and a payload that stops mid-value.
        assertFailsWith<IllegalStateException> {
            WireDecoder.decodeDocument(wirePayload("MKC6", 2.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument(wirePayload("MKC5", 0.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument(wirePayload("MKC6", 0.toByte(), 3.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument(wirePayload("MKC6", 0.toByte(), 1.toByte(), 1, 1))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeDocument(wirePayload("MKC6", 1.toByte(), 1, -2))
        }
    }
}

class BindingMappingTest {
    @Test
    fun extendedKindsDecodeDumpAndWalk() {
        // One document verifies the extended node kinds and their semantic
        // fields through decode, dump, and traversal.
        val source =
            listOf(
                "[foo]: /url \"t\"",
                "",
                ":::note[Title]{kind=demo}",
                "Body",
                ":::",
                "",
                "See [foo], [foo][foo], ![foo] and \$\$x\$\$.",
                "",
                "3. one",
                "4. two",
                "",
                "| a | b | c | d |",
                "| :- | :-: | -: | --- |",
                "| 1 | 2 | 3 | 4 |",
                "",
            ).joinToString("\n")
        val document = Document.parse(source)

        val definition = assertIs<ReferenceDefinition>(document.content[0])
        assertEquals("foo", definition.label)
        assertEquals("/url", definition.destination)
        assertEquals("t", definition.title)

        val block = assertIs<DirectiveBlock>(document.content[1])
        assertIs<DirectiveLabel>(assertNotNull(block.label))
        assertEquals(1, block.content.size)
        assertIs<Paragraph>(block.content.single())
        assertEquals("kind", block.attributes?.first()?.name)

        val inlines = assertIs<Paragraph>(document.content[2]).content
        val references = inlines.filterIsInstance<LinkReference>()
        assertEquals(listOf(ReferenceForm.SHORTCUT, ReferenceForm.FULL), references.map { it.form })
        assertEquals(ReferenceForm.SHORTCUT, inlines.filterIsInstance<ImageReference>().single().form)
        assertEquals(PlacementMode.STANDALONE, inlines.filterIsInstance<Formula>().single().mode)

        // Fully qualified: the model's `List` shadows `kotlin.collections.List`.
        val list = assertIs<com.nouprax.markdown.core.List>(document.content[3])
        assertEquals(ListFlavor.ORDERED, list.flavor)
        assertEquals(3, list.start)

        val table = assertIs<Table>(document.content[4])
        assertEquals(
            listOf(
                TableAlignment.LEFT,
                TableAlignment.CENTER,
                TableAlignment.RIGHT,
                TableAlignment.NONE,
            ),
            table.alignments,
        )

        // The dump and the walk both have a branch per kind, and neither is
        // reached by a corpus that never writes one.
        val dump = document.dump()
        for (fragment in listOf("ReferenceDefinition", "LinkReference", "ImageReference", "DirectiveLabel")) {
            assertTrue(dump.contains(fragment), "dump is missing $fragment")
        }
        val entered = mutableListOf<String>()
        Walker.walk(document) { event, node ->
            if (event == WalkEvent.ENTERING) entered += node::class.simpleName.orEmpty()
        }
        assertTrue(entered.contains("DirectiveLabel"))
        assertTrue(entered.contains("ReferenceDefinition"))
    }

    @Test
    fun aDirectiveBlockWithNoLabelTakesTheOtherArm() {
        val bare = assertIs<DirectiveBlock>(Document.parse(":::note\nBody\n:::\n").content.single())
        assertEquals(null, bare.label)
        assertTrue(bare.dump().contains("children=1"))
    }

    @Test
    fun everyOptionalFieldIsReadBothPresentAndAbsent() {
        // Requirement 14 gives every optional field two answers and the decoder
        // an arm for each. A corpus that always writes the field takes one arm
        // and never the other, so write both and compare them side by side.
        val withEverything =
            Document.parse(
                listOf(
                    "``` kotlin",
                    "code",
                    "```",
                    "",
                    "[a](/u \"t\") ![b](/s \"u\") `c`",
                    "",
                    ":::note{k=v}",
                    "body",
                    ":::",
                    "",
                    "- [x] done",
                    "",
                ).joinToString("\n"),
            )
        val withNothing =
            Document.parse(
                listOf(
                    "```",
                    "code",
                    "```",
                    "",
                    "[a](/u) ![b](/s)",
                    "",
                    ":::note",
                    "body",
                    ":::",
                    "",
                    "- plain",
                    "",
                ).joinToString("\n"),
            )

        val fenced = assertIs<CodeBlock>(withEverything.content[0])
        assertEquals("kotlin", fenced.language)
        assertEquals("kotlin", fenced.info)
        val bare = assertIs<CodeBlock>(withNothing.content[0])
        assertEquals(null, bare.language)
        assertEquals(null, bare.info)

        val rich = assertIs<Paragraph>(withEverything.content[1]).content
        assertEquals("t", rich.filterIsInstance<Link>().single().title)
        assertEquals("u", rich.filterIsInstance<Image>().single().title)
        val plain = assertIs<Paragraph>(withNothing.content[1]).content
        assertEquals(null, plain.filterIsInstance<Link>().single().title)
        assertEquals(null, plain.filterIsInstance<Image>().single().title)

        assertNotNull(assertIs<DirectiveBlock>(withEverything.content[2]).attributes)
        assertEquals(null, assertIs<DirectiveBlock>(withNothing.content[2]).attributes)

        val checked = assertIs<com.nouprax.markdown.core.List>(withEverything.content[3])
        assertEquals(true, checked.items.single().checked)
        val unchecked = assertIs<com.nouprax.markdown.core.List>(withNothing.content[3])
        assertEquals(null, unchecked.items.single().checked)
    }

    @Test
    fun theDumpEscapesEveryCharacterJsonCannotCarryLiterally() {
        // A fenced code block carries its literal through untouched, so it is
        // the one place a test can put every escape the dumper knows.
        val literal = "a\"b\\c\td\u0008e\u000cf\u0001g"
        val dump = Document.parse("```\n$literal\n```\n").dump()
        for (escape in listOf("\\\"", "\\\\", "\\t", "\\b", "\\f", "\\n", "\\u0001")) {
            assertTrue(dump.contains(escape), "dump is missing the escape $escape")
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
