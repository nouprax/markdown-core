package com.nouprax.markdown.core

import kotlin.test.Test
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

        // The owning node keeps its label field separate from block content;
        // the per-node dumper deliberately emits both relations.
        val dump = document.dump()
        for (fragment in listOf("ReferenceDefinition", "LinkReference", "ImageReference", "DirectiveLabel")) {
            assertTrue(dump.contains(fragment), "dump is missing $fragment")
        }
        assertEquals(listOf("Paragraph"), block.content.map { it::class.simpleName })
        assertEquals(listOf("Text"), assertNotNull(block.label).content.map { it::class.simpleName })
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
}
