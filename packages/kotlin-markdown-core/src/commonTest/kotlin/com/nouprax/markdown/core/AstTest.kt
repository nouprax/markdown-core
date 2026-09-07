package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class AstTest {
    @Test
    fun tableColumnWidthsUseCanonicalDecimals() {
        val scope = Document.parse("x").scope
        val widths =
            listOf(
                0.1 to "0.1",
                1e-6 to "0.000001",
                1e-7 to "1e-7",
                1e20 to "100000000000000000000",
                1e21 to "1e+21",
                Double.MIN_VALUE to "5e-324",
                1.2345678901234567 to "1.2345678901234567",
            )
        for ((width, expected) in widths) {
            val table =
                Table(listOf(TableColumn(TableAlignment.NONE, width)), emptyList(), emptyList(), emptyList(), scope)
            assertTrue(table.dump().contains("columns=[none:$expected]"))
        }
    }

    @Test
    fun tablesPreserveGroupsSpansAndDirectBlockContent() {
        val documents = listOf(Document.parse("# head"), Document.parse("body"), Document.parse("---"))
        val rows = documents.map { TableRow(listOf(TableCell(1, 2, it.content, it.scope)), it.scope) }
        val table =
            Table(
                listOf(TableColumn(TableAlignment.LEFT, 0.1), TableColumn(TableAlignment.NONE, null)),
                listOf(rows[0]),
                listOf(rows[1]),
                listOf(rows[2]),
                documents[0].scope,
            )
        assertTrue(table.head[0].cells[0].content[0] is Heading)
        assertTrue(table.foot[0].cells[0].content[0] is ThematicBreak)
        assertEquals(2, table.content[0].cells[0].colspan)
        val visitor = RecordingWalkingVisitor()
        table.walk(visitor)
        assertEquals(
            listOf("entering:Heading", "entering:Paragraph", "entering:ThematicBreak"),
            visitor.events.filter { it in listOf("entering:Heading", "entering:Paragraph", "entering:ThematicBreak") },
        )
        assertTrue(table.dump().contains("columns=[left:0.1,none:null] children=3"))
        assertTrue(table.dump().contains("TableFoot children=1"))
    }

    @Test
    fun publicSchemaIsEmittedByThePerNodeKotlinDumper() {
        val sources =
            listOf(
                "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n\n[ref]: /r \"t\"\n\n[a][ref] ![b][ref]\n",
                "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) :badge[label]{kind=demo} \$x\$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
                "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n:::container[Title]{kind=demo}\nBody\n:::\n",
                "\$\$\ny\n\$\$\n",
                "a <!-- b --> c\n\n<!-- block -->\n",
            )
        val documents = sources.map { Document.parse(it) }
        val kinds = documents.flatMap { dumpKinds(it.dump()) }.toSet()
        assertEquals(
            setOf(
                "Document",
                "Callout",
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
                "Text",
                "SoftBreak",
                "LineBreak",
                "Code",
                "HTML",
                "Comment",
                "Formula",
                "Emphasis",
                "Strong",
                "Strikethrough",
                "Link",
                "Image",
                "Directive",
                "Cite",
                "TableRow",
                "TableCell",
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
        assertEquals("x", (document.content[1] as List).items.single().marker)
        val table = document.content[2] as Table
        assertEquals(listOf(TableAlignment.CENTER), table.columns.map { it.alignment })
        assertEquals(1, table.head.size)
        assertEquals(1, table.content.size)
        assertTrue(table.foot.isEmpty())
        assertTrue(
            table.head
                .single()
                .cells
                .single()
                .scope.start.line > 0,
        )
        val paragraph = document.content[3] as Paragraph
        val link = paragraph.content[0] as Link
        val image = paragraph.content[2] as Image
        assertEquals("/go", (link.dest as Destination.Url).value)
        assertNull(link.title)
        assertEquals("/image", (image.dest as Destination.Url).value)
        assertEquals("title", image.title)
    }

    @Test
    fun visitorDispatchesTableRowsAndCellsAsMarkup() {
        val document = Document.parse("| a |\n| --- |\n| b |\n")
        val table = document.content.single() as Table
        val visitor = RecordingVisitor()
        document.accept(visitor)
        table.accept(visitor)
        table.head.single().accept(visitor)
        table.head
            .single()
            .cells
            .single()
            .accept(visitor)
        table.content.single().accept(visitor)
        table.content
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
            val document = Document.parse(testCase.source)
            assertEquals(testCase.expected, TreeDumper.dump(document), testCase.name)
            assertEquals(testCase.expected, document.dump(), testCase.name)
        }
    }
}

/** The node lines of a dump: value lines (`Citation`, `Footnote`) and group lines are not kinds. */
private fun dumpKinds(dump: String): kotlin.collections.List<String> =
    dump
        .lineSequence()
        .filter { it.contains(" scope=") }
        .map {
            it.trimStart('│', ' ', '├', '└', '─').substringBefore(' ')
        }.filterNot { it == "Citation" || it == "Footnote" }
        .toList()
