package com.nouprax.markdown.core

internal class KindVisitor : Visitor<String> {
    override fun visit(node: Document): String = name(node)

    override fun visit(node: BlockQuote): String = name(node)

    override fun visit(node: Paragraph): String = name(node)

    override fun visit(node: Heading): String = "heading:${node.level}"

    override fun visit(node: ThematicBreak): String = name(node)

    override fun visit(node: List): String = name(node)

    override fun visit(node: ListItem): String = name(node)

    override fun visit(node: CodeBlock): String = name(node)

    override fun visit(node: HTMLBlock): String = name(node)

    override fun visit(node: FormulaBlock): String = name(node)

    override fun visit(node: Table): String = name(node)

    override fun visit(node: TableRow): String = if (node.isHeader) "header" else "row"

    override fun visit(node: TableCell): String = "cell"

    override fun visit(node: DirectiveBlock): String = name(node)

    override fun visit(node: FootnoteDefinition): String = name(node)

    override fun visit(node: Text): String = name(node)

    override fun visit(node: SoftBreak): String = name(node)

    override fun visit(node: LineBreak): String = name(node)

    override fun visit(node: Code): String = name(node)

    override fun visit(node: HTML): String = name(node)

    override fun visit(node: Formula): String = name(node)

    override fun visit(node: Emphasis): String = name(node)

    override fun visit(node: Strong): String = name(node)

    override fun visit(node: Strikethrough): String = name(node)

    override fun visit(node: Link): String = name(node)

    override fun visit(node: Image): String = name(node)

    override fun visit(node: Directive): String = name(node)

    override fun visit(node: FootnoteReference): String = name(node)
}

internal class RecordingVisitor : Visitor<Unit> {
    val visited: MutableList<String> = mutableListOf()

    override fun visit(node: Document): Unit = record(node)

    override fun visit(node: BlockQuote): Unit = record(node)

    override fun visit(node: Paragraph): Unit = record(node)

    override fun visit(node: Heading): Unit = record(node)

    override fun visit(node: ThematicBreak): Unit = record(node)

    override fun visit(node: List): Unit = record(node)

    override fun visit(node: ListItem): Unit = record(node)

    override fun visit(node: CodeBlock): Unit = record(node)

    override fun visit(node: HTMLBlock): Unit = record(node)

    override fun visit(node: FormulaBlock): Unit = record(node)

    override fun visit(node: Table): Unit = record(node)

    override fun visit(node: TableRow): Unit = record(node)

    override fun visit(node: TableCell): Unit = record(node)

    override fun visit(node: DirectiveBlock): Unit = record(node)

    override fun visit(node: FootnoteDefinition): Unit = record(node)

    override fun visit(node: Text): Unit = record(node)

    override fun visit(node: SoftBreak): Unit = record(node)

    override fun visit(node: LineBreak): Unit = record(node)

    override fun visit(node: Code): Unit = record(node)

    override fun visit(node: HTML): Unit = record(node)

    override fun visit(node: Formula): Unit = record(node)

    override fun visit(node: Emphasis): Unit = record(node)

    override fun visit(node: Strong): Unit = record(node)

    override fun visit(node: Strikethrough): Unit = record(node)

    override fun visit(node: Link): Unit = record(node)

    override fun visit(node: Image): Unit = record(node)

    override fun visit(node: Directive): Unit = record(node)

    override fun visit(node: FootnoteReference): Unit = record(node)

    private fun record(node: Markup) {
        visited += name(node)
    }
}

private fun name(node: Markup): String = node::class.simpleName ?: "unknown"
