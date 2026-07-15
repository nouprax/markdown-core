package com.nouprax.markdown.core

internal class KindVisitor : Visitor<String> {
    override fun visitDocument(node: Document): String = name(node)

    override fun visitBlockQuote(node: BlockQuote): String = name(node)

    override fun visitParagraph(node: Paragraph): String = name(node)

    override fun visitHeading(node: Heading): String = "heading:${node.level}"

    override fun visitThematicBreak(node: ThematicBreak): String = name(node)

    override fun visitList(node: List): String = name(node)

    override fun visitListItem(node: ListItem): String = name(node)

    override fun visitCodeBlock(node: CodeBlock): String = name(node)

    override fun visitHTMLBlock(node: HTMLBlock): String = name(node)

    override fun visitFormulaBlock(node: FormulaBlock): String = name(node)

    override fun visitTable(node: Table): String = name(node)

    override fun visitTableRow(node: TableRow): String = if (node.isHeader) "header" else "row"

    override fun visitTableCell(node: TableCell): String = "cell"

    override fun visitDirectiveBlock(node: DirectiveBlock): String = name(node)

    override fun visitFootnoteDefinition(node: FootnoteDefinition): String = name(node)

    override fun visitText(node: Text): String = name(node)

    override fun visitSoftBreak(node: SoftBreak): String = name(node)

    override fun visitLineBreak(node: LineBreak): String = name(node)

    override fun visitCode(node: Code): String = name(node)

    override fun visitHTML(node: HTML): String = name(node)

    override fun visitFormula(node: Formula): String = name(node)

    override fun visitEmphasis(node: Emphasis): String = name(node)

    override fun visitStrong(node: Strong): String = name(node)

    override fun visitStrikethrough(node: Strikethrough): String = name(node)

    override fun visitLink(node: Link): String = name(node)

    override fun visitImage(node: Image): String = name(node)

    override fun visitDirective(node: Directive): String = name(node)

    override fun visitFootnoteReference(node: FootnoteReference): String = name(node)
}

internal class RecordingVisitor : Visitor<Unit> {
    val visited: MutableList<String> = mutableListOf()

    override fun visitDocument(node: Document): Unit = record(node)

    override fun visitBlockQuote(node: BlockQuote): Unit = record(node)

    override fun visitParagraph(node: Paragraph): Unit = record(node)

    override fun visitHeading(node: Heading): Unit = record(node)

    override fun visitThematicBreak(node: ThematicBreak): Unit = record(node)

    override fun visitList(node: List): Unit = record(node)

    override fun visitListItem(node: ListItem): Unit = record(node)

    override fun visitCodeBlock(node: CodeBlock): Unit = record(node)

    override fun visitHTMLBlock(node: HTMLBlock): Unit = record(node)

    override fun visitFormulaBlock(node: FormulaBlock): Unit = record(node)

    override fun visitTable(node: Table): Unit = record(node)

    override fun visitTableRow(node: TableRow): Unit = record(node)

    override fun visitTableCell(node: TableCell): Unit = record(node)

    override fun visitDirectiveBlock(node: DirectiveBlock): Unit = record(node)

    override fun visitFootnoteDefinition(node: FootnoteDefinition): Unit = record(node)

    override fun visitText(node: Text): Unit = record(node)

    override fun visitSoftBreak(node: SoftBreak): Unit = record(node)

    override fun visitLineBreak(node: LineBreak): Unit = record(node)

    override fun visitCode(node: Code): Unit = record(node)

    override fun visitHTML(node: HTML): Unit = record(node)

    override fun visitFormula(node: Formula): Unit = record(node)

    override fun visitEmphasis(node: Emphasis): Unit = record(node)

    override fun visitStrong(node: Strong): Unit = record(node)

    override fun visitStrikethrough(node: Strikethrough): Unit = record(node)

    override fun visitLink(node: Link): Unit = record(node)

    override fun visitImage(node: Image): Unit = record(node)

    override fun visitDirective(node: Directive): Unit = record(node)

    override fun visitFootnoteReference(node: FootnoteReference): Unit = record(node)

    private fun record(node: Markup) {
        visited += name(node)
    }
}

private fun name(node: Markup): String = node::class.simpleName ?: "unknown"
