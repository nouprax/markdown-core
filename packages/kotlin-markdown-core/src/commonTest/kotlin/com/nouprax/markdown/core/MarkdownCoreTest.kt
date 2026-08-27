package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotNull
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
            parse(markdown)
                .content
                .first(),
        )
        assertIs<Paragraph>(
            parse(markdown, ParseOptions(tables = false))
                .content
                .first(),
        )
    }

    @Test
    fun visitorAndWalkerAreTypedAndDepthFirst() {
        val document = parse("# Heading\n\nBody\n")
        val visitor = KindVisitor()
        assertEquals("heading:1", document.content.first().accept(visitor))
        val recordingVisitor = RecordingVisitor()
        Walker.walk(document, recordingVisitor)
        assertEquals("Semantic", recordingVisitor.visited.first())
        assertTrue("Heading" in recordingVisitor.visited && "Text" in recordingVisitor.visited)
    }
}

class UnicodeTest {
    @Test
    fun standardUtf8SurvivesTheNativeBoundary() {
        val document = parse("héllo 🚀 中文\n")
        val paragraph = assertIs<Paragraph>(document.content.first())
        assertEquals("héllo 🚀 中文", assertIs<Text>(paragraph.content.first()).literal)
    }
}

class ErrorsTest {
    @Test
    fun emptyInputIsAValidDocument() {
        assertTrue(
            parse("")
                .content
                .isEmpty(),
        )
    }

    @Test
    fun corruptedNativePayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(byteArrayOf(0x4d, 0x4b, 0x43))
        }
    }

    @Test
    fun everyWireGuardFiresWhenTheNativeSideAnswersOutOfRange() {
        // The two sides of the wire are versioned separately -- the MKC6 bump
        // is that hazard made concrete -- and a decoder that mapped an unknown value
        // instead of refusing it turns a protocol mismatch into a wrong
        // document. Nothing proved any of these fired.
        assertFailsWith<IllegalStateException> { WireKind.from(0) }
        assertFailsWith<IllegalStateException> { WireKind.from(33) }
        assertEquals(WireKind.IMAGE_REFERENCE, WireKind.from(32))

        // A header the decoder accepts, followed by nothing it can read.
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead("MKC6".encodeToByteArray())
        }
    }

    @Test
    fun everyRefusalTheWireReaderCanMakeIsReachedByAPayload() {
        // The reader is one `require` after another and a corpus reaches none
        // of them: every payload the bridge actually writes is well formed. So
        // write the malformed ones by hand. `MKC6` is the magic; the byte after
        // it is the status, and 1 means the payload is an error rather than a
        // document.
        fun payload(vararg parts: Any): ByteArray {
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

        // A native error crosses as a code and a message, which is the only
        // path that builds a ParseException.
        val failure =
            assertFailsWith<ParseException> {
                WireDecoder.decodeRead(payload("MKC6", 1.toByte(), 1, 3, "bad"))
            }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure.code)
        assertEquals("bad", failure.message)
        assertEquals(
            ParseErrorCode.INTERNAL,
            assertFailsWith<ParseException> {
                WireDecoder.decodeRead(payload("MKC6", 1.toByte(), 99, 1, "x"))
            }.code,
        )

        // A status that is neither, a magic from the wrong wire version, a
        // root that is not a document, and a payload that stops mid-value.
        assertFailsWith<IllegalStateException> {
            WireDecoder.decodeRead(payload("MKC6", 2.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC5", 0.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC6", 0.toByte(), 3.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC6", 0.toByte(), 1.toByte(), 1, 1))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC6", 1.toByte(), 1, -2))
        }
    }

    @Test
    fun aParseFailureCarriesItsCodeAndMessageAndNothingElse() {
        // A parse fails only when an allocation does, so no input a caller can
        // write reaches this type through a parse.
        val failure = ParseException(ParseErrorCode.ALLOCATION_FAILED, "out of memory")
        assertEquals(ParseErrorCode.ALLOCATION_FAILED, failure.code)
        assertEquals("out of memory", failure.message)
    }
}

class WireCoverageTest {
    @Test
    fun everyKindThisBranchAddedDecodesDumpsAndWalks() {
        // The kinds Step 7 and Step 9b added -- a definition, both reference
        // spellings and their forms, a directive label -- plus the enum arms
        // nothing else in this suite writes: a standalone formula, an ordered
        // list's start, and every table alignment.
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
        val document = parse(source)

        val definition = assertIs<ReferenceDefinition>(document.content[0])
        assertEquals("foo", definition.label)
        assertEquals("/url", definition.destination)
        assertEquals("t", definition.title)

        val block = assertIs<DirectiveBlock>(document.content[1])
        assertIs<DirectiveLabel>(assertNotNull(block.label))
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
        val bare = assertIs<DirectiveBlock>(parse(":::note\nBody\n:::\n").content.single())
        assertEquals(null, bare.label)
        assertTrue(bare.dump().contains("children=1"))
    }

    @Test
    fun everyOptionalFieldIsReadBothPresentAndAbsent() {
        // Requirement 14 gives every optional field two answers and the decoder
        // an arm for each. A corpus that always writes the field takes one arm
        // and never the other, so write both and compare them side by side.
        val withEverything =
            parse(
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
            parse(
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
        val dump = parse("```\n$literal\n```\n").dump()
        for (escape in listOf("\\\"", "\\\\", "\\t", "\\b", "\\f", "\\n", "\\u0001")) {
            assertTrue(dump.contains(escape), "dump is missing the escape $escape")
        }
    }
}

class OwnershipTest {
    @Test
    fun returnedTreesOutliveEveryNativeDocument() {
        val documents = kotlin.collections.List(300) { parse("# Copy\n\n- [x] item\n") }
        assertTrue(documents.all { it.content.size == 2 })
        assertEquals(1, assertIs<Heading>(documents.last().content.first()).level)
    }

    @Test
    fun readOnlyCollectionsDoNotLeakMutableImplementations() {
        val content = parse("one *two* three\n").content
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
        val document = parse(unit.repeat(5_000))
        assertEquals(10_000, document.content.size)
    }

    @Test
    fun deepBlockQuoteNestingRemainsTraversable() {
        val depth = 128
        var node: Markup =
            parse("> ".repeat(depth) + "leaf\n")
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
                parse("# Copy\n\n- [x] item 🚀\n")
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
        val read = Document(source).seal()
        val concrete = read.concrete
        assertContentEquals(source.encodeToByteArray(), concrete.source)
        assertEquals(15, concrete.lines)
        assertEquals(0, concrete.offset(1))
        assertEquals(14, concrete.offset(3))
        assertFailsWith<IndexOutOfBoundsException> { concrete.offset(0) }
        assertFailsWith<IndexOutOfBoundsException> { concrete.offset(16) }

        // Every line but the first begins after a line ending.
        for (line in 2..concrete.lines) {
            val start = concrete.offset(line)
            assertTrue(start > 0)
            assertEquals('\n'.code.toByte(), concrete.source[start - 1])
        }

        // Nothing native is left: 300 more parses cannot move what was copied.
        repeat(300) { parse("# copy\n") }
        assertContentEquals(source.encodeToByteArray(), concrete.source)
        assertEquals(14, concrete.offset(3))
    }
}
