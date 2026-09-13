package com.nouprax.markdown.core

/** Exhaustive callbacks overloaded by concrete markup type; [Markup.accept] selects the overload. */
public interface Visitor<Result> {
    public fun visit(document: Document): Result

    public fun visit(callout: Callout): Result

    public fun visit(paragraph: Paragraph): Result

    public fun visit(heading: Heading): Result

    public fun visit(thematicBreak: ThematicBreak): Result

    public fun visit(list: List): Result

    public fun visit(listItem: ListItem): Result

    public fun visit(codeBlock: CodeBlock): Result

    public fun visit(htmlBlock: HTMLBlock): Result

    public fun visit(formulaBlock: FormulaBlock): Result

    public fun visit(table: Table): Result

    public fun visit(tableCaption: TableCaption): Result

    public fun visit(tableRow: TableRow): Result

    public fun visit(tableCell: TableCell): Result

    public fun visit(directiveBlock: DirectiveBlock): Result

    public fun visit(directiveLabel: DirectiveLabel): Result

    public fun visit(text: Text): Result

    public fun visit(softBreak: SoftBreak): Result

    public fun visit(lineBreak: LineBreak): Result

    public fun visit(code: Code): Result

    public fun visit(html: HTML): Result

    public fun visit(comment: Comment): Result

    public fun visit(crossLink: CrossLink): Result

    public fun visit(crossEmbedded: CrossEmbedded): Result

    public fun visit(formula: Formula): Result

    public fun visit(emphasis: Emphasis): Result

    public fun visit(strong: Strong): Result

    public fun visit(strikethrough: Strikethrough): Result

    public fun visit(mark: Mark): Result

    public fun visit(insertion: Insertion): Result

    public fun visit(span: Span): Result

    public fun visit(superscript: Superscript): Result

    public fun visit(subscript: Subscript): Result

    public fun visit(definitionList: DefinitionList): Result

    public fun visit(definition: Definition): Result

    public fun visit(link: Link): Result

    public fun visit(embedded: Embedded): Result

    public fun visit(directive: Directive): Result

    public fun visit(cite: Cite): Result
}
