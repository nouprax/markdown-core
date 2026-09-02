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
        // The two sides of the wire are versioned separately -- the MKC8 bump
        // is that hazard made concrete -- and a decoder that mapped an unknown value
        // instead of refusing it turns a protocol mismatch into a wrong
        // document. Nothing proved any of these fired.
        assertFailsWith<IllegalStateException> { WireKind.from(0) }
        assertFailsWith<IllegalStateException> { WireKind.from(33) }
        assertEquals(WireKind.IMAGE_REFERENCE, WireKind.from(32))

        // A header the decoder accepts, followed by nothing it can read.
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead("MKC8".encodeToByteArray())
        }
    }

    @Test
    fun everyRefusalTheWireReaderCanMakeIsReachedByAPayload() {
        // The reader is one `require` after another and a corpus reaches none
        // of them: every payload the bridge actually writes is well formed. So
        // write the malformed ones by hand. `MKC8` is the magic; the byte after
        // it is the status, and 1 means the payload is an error rather than a
        // document; a document then leads with its frame byte, 0 for a whole
        // tree.
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
                WireDecoder.decodeRead(payload("MKC8", 1.toByte(), 1, 3, "bad"))
            }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure.code)
        assertEquals("bad", failure.message)
        assertEquals(
            ParseErrorCode.INTERNAL,
            assertFailsWith<ParseException> {
                WireDecoder.decodeRead(payload("MKC8", 1.toByte(), 99, 1, "x"))
            }.code,
        )

        // A status that is neither, a magic from the wrong wire version, a
        // frame the reader does not know, a delta with nothing to be a delta
        // against, a root that is not a document, and a payload that stops
        // mid-value.
        assertFailsWith<IllegalStateException> {
            WireDecoder.decodeRead(payload("MKC8", 2.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC7", 0.toByte(), 0.toByte()))
        }
        assertFailsWith<IllegalStateException> {
            WireDecoder.decodeRead(payload("MKC8", 0.toByte(), 7.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC8", 0.toByte(), 1.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC8", 0.toByte(), 0.toByte(), 3.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC8", 0.toByte(), 0.toByte(), 1.toByte(), 1, 1))
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(payload("MKC8", 1.toByte(), 1, -2))
        }
    }

    @Test
    fun everyRefusalTheDeltaReaderCanMakeIsReachedByAPayload() {
        // A DELTA frame is a tree of ops against the previous read (#162):
        // SPINE (0xfe) rewrites a container's fields and rebuilds its
        // children from ops, SAME (0xff) reuses the next n of the previous
        // node's children, and any other tag is a kind byte opening a whole
        // node. The reader refuses every way the ops can disagree with the
        // previous read, because a delta that landed on the wrong value would
        // be a wrong document rather than an error.
        fun bytes(vararg parts: Any): kotlin.collections.List<Byte> {
            val out = mutableListOf<Byte>()
            for (part in parts) {
                when (part) {
                    is String -> out += part.encodeToByteArray().toList()
                    is Byte -> out += part
                    is Int -> repeat(4) { shift -> out += ((part shr (shift * 8)) and 0xff).toByte() }
                    is kotlin.collections.List<*> -> out += part.map { it as Byte }
                    else -> error("unsupported payload part")
                }
            }
            return out
        }

        fun identity(
            block: Int,
            ordinal: Int,
        ) = bytes(block, ordinal)
        val scope = bytes(1, 1, 1, 1)

        // A paragraph of one text node, the block's identity on both.
        fun paragraph(
            block: Int,
            literal: String,
        ): kotlin.collections.List<Byte> {
            val text = bytes(14.toByte(), identity(block, 1), scope, literal.length, literal)
            return bytes(3.toByte(), identity(block, 0), scope, 1, text)
        }
        val header = bytes("MKC8", 0.toByte())
        // The previous read: a document holding two paragraphs.
        val previous =
            WireDecoder
                .decodeRead(
                    bytes(
                        header,
                        0.toByte(),
                        1.toByte(),
                        identity(1, 0),
                        scope,
                        2,
                        paragraph(2, "one"),
                        paragraph(3, "two"),
                    ).toByteArray(),
                ).semantic

        fun delta(vararg ops: Any) = bytes(header, 1.toByte(), bytes(*ops)).toByteArray()

        fun root(vararg ops: Any) = delta(0xfe.toByte(), 1.toByte(), identity(1, 0), scope, bytes(*ops))

        // The healthy shapes: the previous children reused as the same
        // objects, one rewritten as a spine, one written whole, in every mix.
        val same = WireDecoder.decodeRead(root(1, 0xff.toByte(), 2), previous).semantic
        assertEquals(2, same.content.size)
        assertTrue(same.content[0] === previous.content[0])
        assertTrue(same.content[1] === previous.content[1])
        val mixed =
            WireDecoder
                .decodeRead(root(3, 0xff.toByte(), 1, paragraph(3, "changed"), paragraph(4, "new")), previous)
                .semantic
        assertEquals(3, mixed.content.size)
        assertTrue(mixed.content[0] === previous.content[0])
        assertEquals("changed", assertIs<Text>(assertIs<Paragraph>(mixed.content[1]).content[0]).literal)
        assertEquals("new", assertIs<Text>(assertIs<Paragraph>(mixed.content[2]).content[0]).literal)
        assertTrue(
            WireDecoder
                .decodeRead(root(0), previous)
                .semantic.content
                .isEmpty(),
        )
        // A whole-tree frame ignores the previous read entirely.
        assertTrue(
            WireDecoder
                .decodeRead(bytes(header, 0.toByte(), 1.toByte(), identity(1, 0), scope, 0).toByteArray(), previous)
                .semantic.content
                .isEmpty(),
        )

        // A delta that does not open with the document's spine, a spine that
        // renames the kind or the identity, a reuse or a rewrite past the
        // previous node's children, a spine on a node that has no children
        // to address, and an op stream that stops early.
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(delta(1.toByte()), previous) }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(delta(0xfe.toByte(), 3.toByte(), identity(1, 0), scope, 0), previous)
        }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(delta(0xfe.toByte(), 1.toByte(), identity(9, 0), scope, 0), previous)
        }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(root(1, 0xff.toByte(), 3), previous) }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(root(2, 0xff.toByte(), 2, 0xfe.toByte()), previous)
        }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(root(1, 0xff.toByte(), -1), previous) }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(root(-1), previous) }
        assertFailsWith<IllegalStateException> {
            WireDecoder.decodeRead(root(1, 0xfe.toByte(), 3.toByte(), identity(2, 0), scope, 0), previous)
        }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(root(1, 0xff.toByte()), previous) }
        assertFailsWith<IllegalArgumentException> {
            WireDecoder.decodeRead(root(1, 0xff.toByte(), 1, 0.toByte()), previous)
        }

        // A table's header row is its FIRST child on the wire -- the engine
        // opens a table with it, and a delta addresses the rows by position
        // -- so a table with no header, with two, or with its header second
        // is refused.
        fun row(
            block: Int,
            header: Boolean,
        ) = bytes(27.toByte(), identity(block, 0), scope, (if (header) 1 else 0).toByte(), 0)

        fun table(vararg rows: kotlin.collections.List<Byte>): ByteArray {
            val node = bytes(11.toByte(), identity(2, 0), scope, 0, rows.size, bytes(*rows))
            return bytes(header, 0.toByte(), 1.toByte(), identity(1, 0), scope, 1, node).toByteArray()
        }
        assertEquals(
            true,
            assertIs<Table>(
                WireDecoder
                    .decodeRead(table(row(3, true), row(4, false)))
                    .semantic.content
                    .single(),
            ).header.isHeader,
        )
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(table(row(3, false))) }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(table(row(3, true), row(4, true))) }
        assertFailsWith<IllegalArgumentException> { WireDecoder.decodeRead(table(row(3, false), row(4, true))) }
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
