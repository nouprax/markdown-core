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
        Walker.walk(document, visitor)
        assertEquals(
            listOf("Document", "Table", "TableRow", "TableCell", "Text", "TableRow", "TableCell", "Text"),
            visitor.visited,
        )
    }

    @Test
    fun allManifestCasesMatchTheSharedCanonicalAstSpec() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            val document = Document.parse(testCase.source, testCase.options)
            assertEquals(testCase.expected, TreeDumper.dump(document), testCase.name)
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
