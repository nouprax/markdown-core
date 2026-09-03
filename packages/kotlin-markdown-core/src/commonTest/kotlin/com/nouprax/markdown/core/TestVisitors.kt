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

    override fun visitDirectiveLabel(node: DirectiveLabel): String = name(node)

    override fun visitFootnoteDefinition(node: FootnoteDefinition): String = name(node)

    override fun visitReferenceDefinition(node: ReferenceDefinition): String = name(node)

    override fun visitLinkReference(node: LinkReference): String = name(node)

    override fun visitImageReference(node: ImageReference): String = name(node)

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

    override fun visitDirectiveLabel(node: DirectiveLabel): Unit = record(node)

    override fun visitFootnoteDefinition(node: FootnoteDefinition): Unit = record(node)

    override fun visitReferenceDefinition(node: ReferenceDefinition): Unit = record(node)

    override fun visitLinkReference(node: LinkReference): Unit = record(node)

    override fun visitImageReference(node: ImageReference): Unit = record(node)

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

internal class RecordingWalkingVisitor(
    private val recordEvents: Boolean = true,
) : WalkingVisitor {
    val events: MutableList<String> = mutableListOf()
    val tableRowKinds: MutableList<Boolean> = mutableListOf()
    var entered: Int = 0
        private set
    var exited: Int = 0
        private set

    private fun record(
        node: Markup,
        phase: WalkPhase,
    ) {
        when (phase) {
            WalkPhase.ENTERING -> entered++
            WalkPhase.EXITING -> exited++
        }
        if (recordEvents) events += "${phase.name.lowercase()}:${name(node)}"
    }

    override fun visitDocument(
        node: Document,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitBlockQuote(
        node: BlockQuote,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitParagraph(
        node: Paragraph,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitHeading(
        node: Heading,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitThematicBreak(
        node: ThematicBreak,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitList(
        node: List,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitListItem(
        node: ListItem,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitCodeBlock(
        node: CodeBlock,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitHTMLBlock(
        node: HTMLBlock,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitFormulaBlock(
        node: FormulaBlock,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitTable(
        node: Table,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitTableRow(
        node: TableRow,
        phase: WalkPhase,
    ) {
        record(node, phase)
        if (phase == WalkPhase.ENTERING) tableRowKinds += node.isHeader
    }

    override fun visitTableCell(
        node: TableCell,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitDirectiveBlock(
        node: DirectiveBlock,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitDirectiveLabel(
        node: DirectiveLabel,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitFootnoteDefinition(
        node: FootnoteDefinition,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitReferenceDefinition(
        node: ReferenceDefinition,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitLinkReference(
        node: LinkReference,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitImageReference(
        node: ImageReference,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitText(
        node: Text,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitSoftBreak(
        node: SoftBreak,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitLineBreak(
        node: LineBreak,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitCode(
        node: Code,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitHTML(
        node: HTML,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitFormula(
        node: Formula,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitEmphasis(
        node: Emphasis,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitStrong(
        node: Strong,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitStrikethrough(
        node: Strikethrough,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitLink(
        node: Link,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitImage(
        node: Image,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitDirective(
        node: Directive,
        phase: WalkPhase,
    ): Unit = record(node, phase)

    override fun visitFootnoteReference(
        node: FootnoteReference,
        phase: WalkPhase,
    ): Unit = record(node, phase)
}
