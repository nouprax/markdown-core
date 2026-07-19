public enum WalkEvent: Sendable {
    case entering
    case exiting
}

public struct MarkupWalker: Sendable {
    public init() {}

    /// Walks the document depth-first, supplying each event with the node's
    /// resolved absolute scope.
    public func walk(
        _ document: Document,
        visit: (WalkEvent, any Markup, Scope) throws -> Void
    ) rethrows {
        try walk(document, from: document, visit: visit)
    }

    /// Walks the subtree rooted at `node`; scopes stay document-absolute.
    public func walk(
        _ document: Document,
        from node: some Markup,
        visit: (WalkEvent, any Markup, Scope) throws -> Void
    ) rethrows {
        try walk(node: node, document: document, visit: visit)
    }

    private func walk(
        node: any Markup,
        document: Document,
        visit: (WalkEvent, any Markup, Scope) throws -> Void
    ) rethrows {
        let scope = document.scope(of: node)
        try visit(.entering, node, scope)
        var visitor = ChildrenVisitor()
        for child in node.accept(&visitor) {
            try walk(node: child, document: document, visit: visit)
        }
        try visit(.exiting, node, scope)
    }
}

private struct ChildrenVisitor: MarkupVisitor {
    mutating func visit(_ node: Document) -> [any Markup] { node.children }

    mutating func visit(_ node: BlockQuote) -> [any Markup] { node.children }

    mutating func visit(_ node: Paragraph) -> [any Markup] { node.children }

    mutating func visit(_ node: Heading) -> [any Markup] { node.children }

    mutating func visit(_: ThematicBreak) -> [any Markup] { [] }

    mutating func visit(_ node: List) -> [any Markup] { node.children }

    mutating func visit(_ node: ListItem) -> [any Markup] { node.children }

    mutating func visit(_: CodeBlock) -> [any Markup] { [] }

    mutating func visit(_: HTMLBlock) -> [any Markup] { [] }

    mutating func visit(_: FormulaBlock) -> [any Markup] { [] }

    mutating func visit(_ node: Table) -> [any Markup] { [node.header] + node.rows }

    mutating func visit(_ node: DirectiveBlock) -> [any Markup] { node.children }

    mutating func visit(_ node: FootnoteDefinition) -> [any Markup] { node.children }

    mutating func visit(_: Text) -> [any Markup] { [] }

    mutating func visit(_: SoftBreak) -> [any Markup] { [] }

    mutating func visit(_: LineBreak) -> [any Markup] { [] }

    mutating func visit(_: Code) -> [any Markup] { [] }

    mutating func visit(_: HTML) -> [any Markup] { [] }

    mutating func visit(_: Formula) -> [any Markup] { [] }

    mutating func visit(_ node: Emphasis) -> [any Markup] { node.children }

    mutating func visit(_ node: Strong) -> [any Markup] { node.children }

    mutating func visit(_ node: Strikethrough) -> [any Markup] { node.children }

    mutating func visit(_ node: Link) -> [any Markup] { node.children }

    mutating func visit(_ node: Image) -> [any Markup] { node.children }

    mutating func visit(_ node: Directive) -> [any Markup] { node.children }

    mutating func visit(_: FootnoteReference) -> [any Markup] { [] }

    mutating func visit(_ node: TableRow) -> [any Markup] { node.cells }

    mutating func visit(_ node: TableCell) -> [any Markup] { node.content }
}
