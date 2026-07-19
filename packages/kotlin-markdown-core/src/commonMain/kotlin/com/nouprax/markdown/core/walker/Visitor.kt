package com.nouprax.markdown.core

public interface Visitor<Result> {
    public fun visit(node: Document): Result

    public fun visit(node: BlockQuote): Result

    public fun visit(node: Paragraph): Result

    public fun visit(node: Heading): Result

    public fun visit(node: ThematicBreak): Result

    public fun visit(node: List): Result

    public fun visit(node: ListItem): Result

    public fun visit(node: CodeBlock): Result

    public fun visit(node: HTMLBlock): Result

    public fun visit(node: FormulaBlock): Result

    public fun visit(node: Table): Result

    public fun visit(node: TableRow): Result

    public fun visit(node: TableCell): Result

    public fun visit(node: DirectiveBlock): Result

    public fun visit(node: FootnoteDefinition): Result

    public fun visit(node: Text): Result

    public fun visit(node: SoftBreak): Result

    public fun visit(node: LineBreak): Result

    public fun visit(node: Code): Result

    public fun visit(node: HTML): Result

    public fun visit(node: Formula): Result

    public fun visit(node: Emphasis): Result

    public fun visit(node: Strong): Result

    public fun visit(node: Strikethrough): Result

    public fun visit(node: Link): Result

    public fun visit(node: Image): Result

    public fun visit(node: Directive): Result

    public fun visit(node: FootnoteReference): Result
}
