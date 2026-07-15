package com.nouprax.markdown.core

public interface Visitor<Result> {
    public fun visitDocument(node: Document): Result

    public fun visitBlockQuote(node: BlockQuote): Result

    public fun visitParagraph(node: Paragraph): Result

    public fun visitHeading(node: Heading): Result

    public fun visitThematicBreak(node: ThematicBreak): Result

    public fun visitList(node: List): Result

    public fun visitListItem(node: ListItem): Result

    public fun visitCodeBlock(node: CodeBlock): Result

    public fun visitHTMLBlock(node: HTMLBlock): Result

    public fun visitFormulaBlock(node: FormulaBlock): Result

    public fun visitTable(node: Table): Result

    public fun visitTableRow(node: TableRow): Result

    public fun visitTableCell(node: TableCell): Result

    public fun visitDirectiveBlock(node: DirectiveBlock): Result

    public fun visitFootnoteDefinition(node: FootnoteDefinition): Result

    public fun visitText(node: Text): Result

    public fun visitSoftBreak(node: SoftBreak): Result

    public fun visitLineBreak(node: LineBreak): Result

    public fun visitCode(node: Code): Result

    public fun visitHTML(node: HTML): Result

    public fun visitFormula(node: Formula): Result

    public fun visitEmphasis(node: Emphasis): Result

    public fun visitStrong(node: Strong): Result

    public fun visitStrikethrough(node: Strikethrough): Result

    public fun visitLink(node: Link): Result

    public fun visitImage(node: Image): Result

    public fun visitDirective(node: Directive): Result

    public fun visitFootnoteReference(node: FootnoteReference): Result
}
