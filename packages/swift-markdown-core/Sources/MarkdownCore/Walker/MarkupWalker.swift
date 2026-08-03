/// Whether a walk callback fires on entering or leaving a node.
public enum WalkEvent: Sendable {
    case entering
    case exiting
}

/// A read-only depth-first traversal that supplies each event with the
/// node's resolved absolute scope. Traversal is iterative: documents walk
/// at any nesting depth that parses.
public struct MarkupWalker: Sendable {
    /// Creates a walker; walkers are stateless and reusable.
    public init() {}

    /// Walks the document depth-first and dispatches each node to `visitor`
    /// once in preorder. This overload is structural and never resolves
    /// scopes, so it can traverse a retained snapshot even if that snapshot
    /// was superseded before scope materialization.
    public func walk<V: MarkupVisitor>(
        _ document: Document,
        visitor: inout V
    ) where V.Result == Void {
        var stack: [any Markup] = [document]
        while let node = stack.popLast() {
            _ = node.accept(&visitor)
            var childrenVisitor = ChildrenVisitor()
            for child in node.accept(&childrenVisitor).reversed() {
                stack.append(child)
            }
        }
    }

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

    private enum Frame {
        case enter(any Markup)
        case exit(any Markup, Scope)
    }

    private func walk(
        node: any Markup,
        document: Document,
        visit: (WalkEvent, any Markup, Scope) throws -> Void
    ) rethrows {
        // Nesting depth is input-controlled, so the traversal runs over an
        // explicit frame stack: a document that parsed must also walk,
        // whatever its depth. Children are pushed reversed so pops preserve
        // the recursive `.entering`/`.exiting` order exactly.
        var stack: [Frame] = [.enter(node)]
        while let frame = stack.popLast() {
            switch frame {
            case .exit(let node, let scope):
                try visit(.exiting, node, scope)
            case .enter(let node):
                let scope = document.scope(of: node)
                try visit(.entering, node, scope)
                stack.append(.exit(node, scope))
                var visitor = ChildrenVisitor()
                for child in node.accept(&visitor).reversed() {
                    stack.append(.enter(child))
                }
            }
        }
    }
}

private struct ChildrenVisitor: MarkupVisitor {
    mutating func visit(_ node: Document) -> [any Markup] { node.content }

    mutating func visit(_ node: BlockQuote) -> [any Markup] { node.content }

    mutating func visit(_ node: Paragraph) -> [any Markup] { node.content }

    mutating func visit(_ node: Heading) -> [any Markup] { node.content }

    mutating func visit(_: ThematicBreak) -> [any Markup] { [] }

    mutating func visit(_ node: List) -> [any Markup] { node.items }

    mutating func visit(_ node: ListItem) -> [any Markup] { node.content }

    mutating func visit(_: CodeBlock) -> [any Markup] { [] }

    mutating func visit(_: HTMLBlock) -> [any Markup] { [] }

    mutating func visit(_: FormulaBlock) -> [any Markup] { [] }

    mutating func visit(_ node: Table) -> [any Markup] { [node.header] + node.rows }

    mutating func visit(_ node: TableRow) -> [any Markup] { node.cells }

    mutating func visit(_ node: TableCell) -> [any Markup] { node.content }

    mutating func visit(_ node: DirectiveBlock) -> [any Markup] {
        (node.label.map { [$0 as any Markup] } ?? []) + node.content
    }

    mutating func visit(_ node: DirectiveLabel) -> [any Markup] { node.content }

    mutating func visit(_ node: FootnoteDefinition) -> [any Markup] { node.content }

    mutating func visit(_: Text) -> [any Markup] { [] }

    mutating func visit(_: SoftBreak) -> [any Markup] { [] }

    mutating func visit(_: LineBreak) -> [any Markup] { [] }

    mutating func visit(_: Code) -> [any Markup] { [] }

    mutating func visit(_: HTML) -> [any Markup] { [] }

    mutating func visit(_: Formula) -> [any Markup] { [] }

    mutating func visit(_ node: Emphasis) -> [any Markup] { node.content }

    mutating func visit(_ node: Strong) -> [any Markup] { node.content }

    mutating func visit(_ node: Strikethrough) -> [any Markup] { node.content }

    mutating func visit(_ node: Link) -> [any Markup] { node.content }

    mutating func visit(_ node: Image) -> [any Markup] { node.content }

    mutating func visit(_ node: Directive) -> [any Markup] {
        node.label.map { [$0 as any Markup] } ?? []
    }

    mutating func visit(_: FootnoteReference) -> [any Markup] { [] }

    mutating func visit(_: ReferenceDefinition) -> [any Markup] { [] }
    mutating func visit(_ node: LinkReference) -> [any Markup] { node.content }
    mutating func visit(_ node: ImageReference) -> [any Markup] { node.content }
    mutating func visit(_: CrossLink) -> [any Markup] { [] }

    mutating func visit(_: Embed) -> [any Markup] { [] }
}
