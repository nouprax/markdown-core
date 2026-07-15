public protocol MarkupVisitor {
    associatedtype Result
    mutating func visit(_ node: Document) -> Result
    mutating func visit(_ node: BlockQuote) -> Result
    mutating func visit(_ node: Paragraph) -> Result
    mutating func visit(_ node: Heading) -> Result
    mutating func visit(_ node: ThematicBreak) -> Result
    mutating func visit(_ node: List) -> Result
    mutating func visit(_ node: ListItem) -> Result
    mutating func visit(_ node: CodeBlock) -> Result
    mutating func visit(_ node: HTMLBlock) -> Result
    mutating func visit(_ node: FormulaBlock) -> Result
    mutating func visit(_ node: Table) -> Result
    mutating func visit(_ node: DirectiveBlock) -> Result
    mutating func visit(_ node: FootnoteDefinition) -> Result
    mutating func visit(_ node: Text) -> Result
    mutating func visit(_ node: SoftBreak) -> Result
    mutating func visit(_ node: LineBreak) -> Result
    mutating func visit(_ node: Code) -> Result
    mutating func visit(_ node: HTML) -> Result
    mutating func visit(_ node: Formula) -> Result
    mutating func visit(_ node: Emphasis) -> Result
    mutating func visit(_ node: Strong) -> Result
    mutating func visit(_ node: Strikethrough) -> Result
    mutating func visit(_ node: Link) -> Result
    mutating func visit(_ node: Image) -> Result
    mutating func visit(_ node: Directive) -> Result
    mutating func visit(_ node: FootnoteReference) -> Result
    mutating func visit(_ node: TableRow) -> Result
    mutating func visit(_ node: TableCell) -> Result
}
