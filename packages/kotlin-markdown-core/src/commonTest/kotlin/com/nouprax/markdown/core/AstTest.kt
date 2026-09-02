package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class AstTest {
    @Test
    fun publicSchemaIsEmittedByThePerNodeKotlinDumper() {
        val sources =
            listOf(
                "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n\n[ref]: /r \"t\"\n\n[a][ref] ![b][ref]\n",
                "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) :badge[label]{kind=demo} \$x\$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
                "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
                "\$\$\ny\n\$\$\n",
            )
        val documents = sources.map { Document.parse(it) }
        val kinds = documents.flatMap { dumpKinds(it.dump()) }.toSet()
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
                "TableRow",
                "TableCell",
                "ReferenceDefinition",
                "LinkReference",
                "ImageReference",
            ),
            kinds,
        )
        assertTrue(documents.all { it.scope.start == Position(1, 1) })
    }

    @Test
    fun fieldsNullabilityAndTypedTableNodesAreMapped() {
        val document =
            Document
                .parse(
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
    fun visitorDispatchesTableRowsAndCellsAsMarkup() {
        val document = Document.parse("| a |\n| --- |\n| b |\n")
        val table = document.content.single() as Table
        val visitor = RecordingVisitor()
        document.accept(visitor)
        table.accept(visitor)
        table.header.accept(visitor)
        table.header.cells
            .single()
            .accept(visitor)
        table.rows.single().accept(visitor)
        table.rows
            .single()
            .cells
            .single()
            .accept(visitor)
        assertEquals(
            listOf("Document", "Table", "TableRow", "TableCell", "TableRow", "TableCell"),
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

private fun dumpKinds(dump: String): kotlin.collections.List<String> =
    dump
        .lineSequence()
        .filter(String::isNotEmpty)
        .map {
            it.trimStart('│', ' ', '├', '└', '─').substringBefore(' ')
        }.toList()
