package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

class ApiTest {
    @Test
    fun theDialectHasNoSwitches() {
        // One witness per feature that used to sit behind a `ParseOptions`
        // field, and one for the substitution smart punctuation used to make:
        // a plain parse recognizes all of them, and there is nothing to pass.
        assertIs<Table>(
            Document
                .parse("| a |\n| --- |\n| b |\n")
                .content
                .first(),
        )
        val witnesses =
            listOf(
                "~~x~~\n" to "Strikethrough scope=",
                "www.example.com\n" to "Link scope=",
                "- [x] task\n" to "marker=\"x\"",
                "ref[^a]\n\n[^a]: note\n" to "Cite scope=",
                "\$x\$\n" to "Formula scope=",
                ":badge[label]\n" to "Directive scope=",
                "\"quotes\" -- ...\n" to "literal=\"\\\"quotes\\\" -- ...\"",
            )
        for ((source, witness) in witnesses) {
            assertTrue(Document.parse(source).dump().contains(witness), "expected $witness for $source")
        }
    }

    @Test
    fun visitorIsTypedAndDispatchesByNodeKind() {
        val document = Document.parse("# Heading\n\nBody\n")
        val visitor = KindVisitor()
        assertEquals("heading:1", document.content.first().accept(visitor))
        assertEquals("Document", document.accept(visitor))
        assertEquals("Paragraph", document.content.last().accept(visitor))
    }

    @Test
    fun walkingVisitorIsTypedAndPreservesOwnedFieldSemantics() {
        val block = assertIs<DirectiveBlock>(Document.parse(":::note[Title]\nBody\n:::\n").content.single())
        val visitor = RecordingWalkingVisitor()
        block.walk(visitor)

        assertEquals(
            listOf(
                "entering:DirectiveBlock",
                "entering:DirectiveLabel",
                "entering:Text",
                "exiting:Text",
                "exiting:DirectiveLabel",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:DirectiveBlock",
            ),
            visitor.events,
        )
        assertEquals(listOf("Paragraph"), block.content.map { it::class.simpleName })

        val table = assertIs<Table>(Document.parse("| a |\n| --- |\n| b |\n").content.single())
        val tableVisitor = RecordingWalkingVisitor()
        table.walk(tableVisitor)
        assertEquals(listOf(true, false), tableVisitor.tableRowKinds)
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
}

class BindingMappingTest {
    @Test
    fun everyQuoteContainerIsAMetadataFreeCallout() {
        // M3: the kind is `Callout`; the metadata rule that fills variant,
        // collapsed, and title in lands with O8, so every callout reads as
        // metadata-free and dumps its fields as such.
        val document = Document.parse("> quote\n")
        val callout = assertIs<Callout>(document.content.single())
        assertEquals(null, callout.variant)
        assertEquals(null, callout.collapsed)
        assertEquals(null, callout.title)
        assertEquals(1, callout.content.size)
        assertEquals(
            "Document scope=1:1..1:7 children=1\n" +
                "└── Callout scope=1:1..1:7 variant=null collapsed=null children=1\n" +
                "    └── Paragraph scope=1:3..1:7 children=1\n" +
                "        └── Text scope=1:3..1:7 literal=\"quote\" children=0\n",
            document.dump(),
        )
    }

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

        // M2: the definition produces no node, and every reference form is
        // the Link or Image it names, with the definition's destination and
        // title.
        val block = assertIs<DirectiveBlock>(document.content[0])
        assertIs<DirectiveLabel>(assertNotNull(block.label))
        assertEquals(1, block.content.size)
        assertIs<Paragraph>(block.content.single())
        assertEquals("kind", block.attributes?.first()?.name)

        val inlines = assertIs<Paragraph>(document.content[1]).content
        val links = inlines.filterIsInstance<Link>()
        assertEquals(2, links.size)
        for (link in links) {
            assertEquals("/url", assertIs<Destination.Url>(link.dest).value)
            assertEquals("t", link.title)
        }
        assertSame(links[0].dest, links[1].dest, "one definition materializes one resource")
        val image = inlines.filterIsInstance<Image>().single()
        assertEquals("/url", assertIs<Destination.Url>(image.dest).value)
        assertSame(links[0].dest, image.dest, "an image reference shares the definition's resource too")
        assertEquals(PlacementMode.STANDALONE, inlines.filterIsInstance<Formula>().single().mode)

        // Fully qualified: the model's `List` shadows `kotlin.collections.List`.
        val list = assertIs<com.nouprax.markdown.core.List>(document.content[2])
        assertEquals(ListFlavor.ORDERED, list.flavor)
        assertEquals(3, list.start)

        val table = assertIs<Table>(document.content[3])
        assertEquals(
            listOf(
                TableAlignment.LEFT,
                TableAlignment.CENTER,
                TableAlignment.RIGHT,
                TableAlignment.NONE,
            ),
            table.alignments,
        )

        // The owning node keeps its label field separate from block content;
        // the per-node dumper deliberately emits both relations.
        val dump = document.dump()
        for (fragment in listOf("Link scope=", "Image scope=", "DirectiveLabel")) {
            assertTrue(dump.contains(fragment), "dump is missing $fragment")
        }
        assertEquals(listOf("Paragraph"), block.content.map { it::class.simpleName })
        assertEquals(listOf("Text"), assertNotNull(block.label).content.map { it::class.simpleName })
    }

    @Test
    fun citationsAreValuesAndTheDocumentOwnsItsFootnotes() {
        // M4: an inherited call is a one-item cite naming its footnote by id
        // with empty affixes; the footnote is a value the document owns, never
        // content, and the walk reaches it after the content. Repeated calls
        // share one footnote: the first definition of an id is the one they
        // resolve to, and a later definition of the same id is a footnote
        // after it, as the inherited grammar parses it.
        val document = Document.parse("[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n")
        val cites = assertIs<Paragraph>(document.content.single()).content.filterIsInstance<Cite>()
        assertEquals(2, cites.size)
        for (cite in cites) {
            val citation = cite.citations.single()
            assertEquals("a", assertIs<CitationReferent.Footnote>(citation.referent).id)
            assertEquals(emptyList(), citation.prefix)
            assertEquals(emptyList(), citation.suffix)
        }
        assertEquals(listOf("a", "a"), document.footnotes.map { it.id })
        val footnote = document.footnotes.first()
        assertEquals(Scope(Position(3, 1), Position(4, 0)), footnote.scope)
        assertEquals("once", assertIs<Text>(assertIs<Paragraph>(footnote.content.single()).content.single()).literal)
        val later = document.footnotes.last()
        assertEquals(Scope(Position(5, 1), Position(5, 11)), later.scope)
        assertEquals("twice", assertIs<Text>(assertIs<Paragraph>(later.content.single()).content.single()).literal)
        assertTrue(
            document.dump().endsWith(
                "└── Footnote scope=5:1..5:11 id=\"a\" children=1\n" +
                    "    └── Paragraph scope=5:7..5:11 children=1\n" +
                    "        └── Text scope=5:7..5:11 literal=\"twice\" children=0\n",
            ),
        )
        assertTrue(document.dump().startsWith("Document scope=1:1..5:11 children=1\n"))

        val visitor = RecordingWalkingVisitor()
        document.walk(visitor)
        val cite = listOf("entering:Cite", "entering:Citation", "exiting:Citation", "exiting:Cite")
        val footnoteEvents =
            listOf(
                "entering:Footnote",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:Footnote",
            )
        assertEquals(
            listOf("entering:Document", "entering:Paragraph") + cite + listOf("entering:Text", "exiting:Text") + cite +
                listOf("exiting:Paragraph") + footnoteEvents + footnoteEvents + listOf("exiting:Document"),
            visitor.events,
        )
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
        assertEquals("x", checked.items.single().marker)
        assertEquals(true, checked.items.single().task)
        assertEquals(true, checked.items.single().completed)
        val unchecked = assertIs<com.nouprax.markdown.core.List>(withNothing.content[3])
        assertEquals(null, unchecked.items.single().marker)
        assertEquals(false, unchecked.items.single().task)
        assertEquals(false, unchecked.items.single().completed)
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
    fun uncappedListNestingRemainsTraversable() {
        val depth = 10_000
        val document = Document.parse("- ".repeat(depth) + "leaf\n")
        val visitor = RecordingWalkingVisitor(recordEvents = false)
        document.walk(visitor)
        assertEquals(visitor.entered, visitor.exited)
        assertTrue(visitor.entered > depth * 2)

        var node: Markup =
            document
                .content
                .single()
        repeat(depth) {
            val list = assertIs<List>(node)
            node =
                list.items
                    .single()
                    .content
                    .single()
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

    @Test
    fun everyOccurrenceOfOneDefinitionMaterializesOneResource() {
        // M2: the C tree shares one resource across every occurrence of a
        // definition, the JNI payload sends it once, and both decoders reuse
        // the one value they built for it.
        val destination = "/" + "u".repeat(1024)
        val count = 5_000
        val document = Document.parse("[a]: $destination\n\n" + "[a]\n\n".repeat(count))
        val links = document.content.map { assertIs<Link>(assertIs<Paragraph>(it).content.single()) }
        assertEquals(count, links.size)
        val first = links.first().dest
        assertEquals(destination, assertIs<Destination.Url>(first).value)
        assertTrue(links.all { it.dest === first }, "every occurrence materializes the one resource")
    }
}
