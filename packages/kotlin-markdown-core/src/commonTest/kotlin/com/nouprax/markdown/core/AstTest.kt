package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class AstTest {
    @Test
    fun publicSchemaIsReachableThroughKotlinValues() {
        val sources =
            listOf(
                "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n",
                "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) :badge[label]{kind=demo} \$x\$ [^n] [[cross_link]] ![[embed]]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
                "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
                "\$\$\ny\n\$\$\n",
                "[full][target] and [target][] and [target] plus ![alt][target]\n\n[target]: /ref \"ref title\"\n",
            )
        val documents = sources.map { Document(it) }
        val values = documents.flatMap(::flatten)
        assertEquals(
            setOf(
                "Document",
                "BlockQuote",
                "Paragraph",
                "Heading",
                "ThematicBreak",
                "List",
                "ListItem",
                "CodeBlock",
                "HTMLBlock",
                "FormulaBlock",
                "Table",
                "TableRow",
                "TableCell",
                "DirectiveBlock",
                "DirectiveLabel",
                "FootnoteDefinition",
                "Text",
                "SoftBreak",
                "LineBreak",
                "Code",
                "HTML",
                "Formula",
                "Emphasis",
                "Strong",
                "Strikethrough",
                "Link",
                "Image",
                "Directive",
                "FootnoteReference",
                "CrossLink",
                "Embed",
                "ReferenceDefinition",
                "LinkReference",
                "ImageReference",
            ),
            values.mapNotNullTo(mutableSetOf()) { it::class.simpleName },
        )
        assertTrue(documents.all { it.scope.start == Position(1, 1) })
    }

    @Test
    fun fieldsNullabilityAndTypedTableNodesAreMapped() {
        val document =
            Document(
                "3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image \"title\")\n",
            )
        val ordered = document.content[0] as List
        assertEquals(ListFlavor.ORDERED, ordered.flavor)
        assertEquals(3, ordered.start)
        assertEquals(true, (document.content[1] as List).items.single().checked)
        val table = document.content[2] as Table
        assertEquals(listOf(TableAlignment.CENTER), table.alignments)
        assertTrue(table.header.isHeader)
        assertTrue(table.rows.all { !it.isHeader })
        assertTrue(
            table.header.cells
                .single()
                .scope.start.line > 0,
        )
        val paragraph = document.content[3] as Paragraph
        val link = paragraph.content[0] as Link
        val image = paragraph.content[2] as Image
        assertEquals("/go", link.destination)
        assertNull(link.title)
        assertEquals("/image", image.source)
        assertEquals("title", image.title)
    }

    @Test
    fun referenceNodesCarryTheirLabelFormAndDefinition() {
        val document =
            Document(
                "[full][target] and [target][] and [target] plus ![alt][target]\n\n[target]: /ref \"ref title\"\n",
            )
        val paragraph = document.content[0] as Paragraph
        val references = paragraph.content.filterIsInstance<LinkReference>()
        // The three source forms resolve identically and are kept apart only
        // because the tree says what was written.
        assertEquals(kotlin.collections.List(3) { "target" }, references.map { it.label })
        assertEquals(
            listOf(ReferenceForm.FULL, ReferenceForm.COLLAPSED, ReferenceForm.SHORTCUT),
            references.map { it.form },
        )
        assertEquals("full", (references[0].content.single() as Text).literal)

        val image = paragraph.content.filterIsInstance<ImageReference>().single()
        assertEquals("target", image.label)
        assertEquals(ReferenceForm.FULL, image.form)
        assertEquals("alt", (image.content.single() as Text).literal)

        // The destination is stated once, at the definition, and nowhere else.
        val definition = document.content[1] as ReferenceDefinition
        assertEquals("target", definition.label)
        assertEquals("/ref", definition.destination)
        assertEquals("ref title", definition.title)

        // Value identity is the id/revision pair, as for every other kind:
        // equal to itself, unequal to another node, and never equal to a
        // non-Markup value.
        for (node in listOf<Markup>(definition, references[0], image)) {
            assertEquals(node, node)
            assertEquals(node.hashCode(), node.hashCode())
            assertTrue(node != paragraph)
            assertTrue(!node.equals("not markup"))
        }
    }

    @Test
    fun walkerDispatchesTableRowsAndCellsAsMarkup() {
        val document = Document("| a |\n| --- |\n| b |\n")
        val visitor = RecordingVisitor()
        MarkupWalker.walk(document, visitor)
        assertEquals(
            listOf("Document", "Table", "TableRow", "TableCell", "Text", "TableRow", "TableCell", "Text"),
            visitor.visited,
        )
    }

    @Test
    fun directiveLabelsAreTypedMarkupChildren() {
        val source =
            "Inline :badge[label] then :plain and :empty[].\n\n" +
                ":::note[Title]\nBody\n:::\n\n" +
                "::plain\n\n" +
                "::empty[]\n"
        val document = Document(source)
        val paragraph = document.content[0] as Paragraph
        val directives = paragraph.content.filterIsInstance<Directive>()
        val populated = directives[0].label
        assertNotNull(populated)
        assertEquals("label", (populated.content.single() as Text).literal)
        assertNull(directives[1].label)
        val emptyInlineLabel = directives[2].label
        assertNotNull(emptyInlineLabel)
        assertTrue(emptyInlineLabel.content.isEmpty())

        val block = document.content[1] as DirectiveBlock
        val blockLabel = block.label
        assertNotNull(blockLabel)
        assertEquals("Title", (blockLabel.content.single() as Text).literal)
        assertTrue(block.content.single() is Paragraph)
        assertNull((document.content[2] as DirectiveBlock).label)
        val emptyBlockLabel = (document.content[3] as DirectiveBlock).label
        assertNotNull(emptyBlockLabel)
        assertTrue(emptyBlockLabel.content.isEmpty())

        val visitor = RecordingVisitor()
        MarkupWalker.walk(Document(":badge[label]\n"), visitor)
        assertEquals(
            listOf("Document", "Paragraph", "Directive", "DirectiveLabel", "Text"),
            visitor.visited,
        )
    }

    @Test
    fun directiveLabelRelinksAsOneStableMarkupChild() {
        val first = Document(":::note[Title]\nBody\n:::\n")
        val firstBlock = first.content.single() as DirectiveBlock
        val firstLabel = assertNotNull(firstBlock.label)

        first.append("\nTail\n").use { second ->
            val secondBlock = second.content.first() as DirectiveBlock
            val secondLabel = assertNotNull(secondBlock.label)
            // The label did not change, so it keeps its identity and compares
            // equal — which is what a reactive framework reads. Equality is
            // the (id, revision) pair, never object identity: a document's
            // projection is a function of its text, not of how the caller
            // reached it.
            assertEquals<Markup>(firstLabel, secondLabel)
            assertEquals<Markup>(secondLabel, assertNotNull(second.node(firstLabel.id)))
        }
    }

    @Test
    fun subtreeDumpsRebaseScopesToTheSubtreeOrigin() {
        val document = Document("Lead\n\n# Heading\n")
        // The document-rooted subtree form is the plain dump.
        assertEquals(document.dump(), MarkupDumper.dump(document, document))
        assertEquals(document.dump(), document.dump(document))
        // A subtree dump prints scopes with the subtree as origin: the
        // root's start line becomes line 1 and columns are unchanged.
        val heading = document.content[1]
        val subtree = MarkupDumper.dump(document, heading)
        assertTrue(subtree.startsWith("Heading scope=1:1..1:9 level=1"), subtree)
        assertEquals(subtree, document.dump(heading))
    }

    @Test
    fun subtreeDumpsPrintPositionFreeMarkersUnchanged() {
        val document = Document("Lead\n\nhard  \nbreak\n")
        val paragraph = document.content[1]
        val subtree = document.dump(paragraph)
        assertTrue(subtree.startsWith("Paragraph scope=1:1..2:"), subtree)
        assertTrue("LineBreak scope=0:0..0:0" in subtree, subtree)
    }

    @Test
    fun allManifestCasesMatchTheSharedCanonicalAstSpec() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            val document = Document(testCase.source, testCase.options)
            assertEquals(testCase.expected, MarkupDumper.dump(document), testCase.name)
            assertEquals(testCase.expected, document.dump(), testCase.name)
        }
    }
}

private fun flatten(root: Any): kotlin.collections.List<Any> =
    listOf(root) +
        when (root) {
            is Document -> root.content.flatMap(::flatten)
            is BlockQuote -> root.content.flatMap(::flatten)
            is Paragraph -> root.content.flatMap(::flatten)
            is Heading -> root.content.flatMap(::flatten)
            is List -> root.items.flatMap(::flatten)
            is ListItem -> root.content.flatMap(::flatten)
            is Table -> flatten(root.header) + root.rows.flatMap(::flatten)
            is TableRow -> root.cells.flatMap(::flatten)
            is TableCell -> root.content.flatMap(::flatten)
            is DirectiveBlock -> (listOfNotNull(root.label) + root.content).flatMap(::flatten)
            is DirectiveLabel -> root.content.flatMap(::flatten)
            is FootnoteDefinition -> root.content.flatMap(::flatten)
            is Emphasis -> root.content.flatMap(::flatten)
            is Strong -> root.content.flatMap(::flatten)
            is Strikethrough -> root.content.flatMap(::flatten)
            is Link -> root.content.flatMap(::flatten)
            is Image -> root.content.flatMap(::flatten)
            is LinkReference -> root.content.flatMap(::flatten)
            is ImageReference -> root.content.flatMap(::flatten)
            is Directive -> listOfNotNull(root.label).flatMap(::flatten)
            else -> emptyList()
        }
