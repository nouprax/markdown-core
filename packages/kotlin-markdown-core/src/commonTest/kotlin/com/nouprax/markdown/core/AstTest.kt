package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class AstTest {
    @Test
    fun publicSchemaIsReachableThroughKotlinValues() {
        val sources =
            listOf(
                "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n",
                "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) :badge[label]{kind=demo} \$x\$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
                "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
                "\$\$\ny\n\$\$\n",
            )
        val documents = sources.map { Document.parse(it) }
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
                "DirectiveBlock",
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
                "TableRow",
                "TableCell",
            ),
            values.mapNotNullTo(mutableSetOf()) { it::class.simpleName },
        )
        assertTrue(documents.all { it.scope(it).start == Position(1, 1) })
    }

    @Test
    fun fieldsNullabilityAndTypedTableNodesAreMapped() {
        val document =
            Document.parse(
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
            document
                .scope(table.header.cells.single())
                .start.line > 0,
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
    fun walkerDispatchesTableRowsAndCellsAsMarkup() {
        val document = Document.parse("| a |\n| --- |\n| b |\n")
        val visitor = RecordingVisitor()
        MarkupWalker.walk(document, visitor)
        assertEquals(
            listOf("Document", "Table", "TableRow", "TableCell", "Text", "TableRow", "TableCell", "Text"),
            visitor.visited,
        )
    }

    @Test
    fun subtreeDumpsRebaseScopesToTheSubtreeOrigin() {
        val document = Document.parse("Lead\n\n# Heading\n")
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
        val document = Document.parse("Lead\n\nhard  \nbreak\n")
        val paragraph = document.content[1]
        val subtree = document.dump(paragraph)
        assertTrue(subtree.startsWith("Paragraph scope=1:1..2:"), subtree)
        assertTrue("LineBreak scope=0:0..0:0" in subtree, subtree)
    }

    @Test
    fun allManifestCasesMatchTheSharedCanonicalAstSpec() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            val document = Document.parse(testCase.source, testCase.options)
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
            is DirectiveBlock -> (root.label.orEmpty() + root.content).flatMap(::flatten)
            is FootnoteDefinition -> root.content.flatMap(::flatten)
            is Emphasis -> root.content.flatMap(::flatten)
            is Strong -> root.content.flatMap(::flatten)
            is Strikethrough -> root.content.flatMap(::flatten)
            is Link -> root.content.flatMap(::flatten)
            is Image -> root.content.flatMap(::flatten)
            is Directive -> root.label.orEmpty().flatMap(::flatten)
            else -> emptyList()
        }
