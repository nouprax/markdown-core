/// Which side of a node a walk is on.
///
/// Every node gets both, in preorder: a leaf's ``entering`` is followed
/// immediately by its ``exiting``.
public enum WalkEvent: Sendable {
    /// The node, before its children.
    case entering
    /// The same node, after them.
    case exiting
}

/// A depth-first walk over a parsed tree.
///
/// It reads a value tree that owns everything it contains, so there is no
/// native handle to outlive and nothing a walk can do is unsafe. Mutation is
/// not offered: the model is immutable.
public struct Walker: Sendable {
    /// Creates a walker. It carries no state between walks.
    public init() {}

    /// Walks `root` and everything under it, in source order.
    ///
    /// `visit` is called twice per node — see ``WalkEvent`` — and a throw from
    /// it abandons the walk and propagates.
    public func walk(_ root: some Markup, visit: (WalkEvent, any Markup) throws -> Void) rethrows {
        try walk(node: root, visit: visit)
    }

    private func walk(
        node: any Markup,
        visit: (WalkEvent, any Markup) throws -> Void
    ) rethrows {
        try visit(.entering, node)
        var visitor = ChildrenVisitor()
        for child in node.accept(&visitor) {
            try walk(node: child, visit: visit)
        }
        try visit(.exiting, node)
    }
}

/// Every child of a node, in the order the C tree holds them. `Walker` walks
/// with it, so a walk and a hand-written descent cannot disagree about what a
/// node's children are.
struct ChildrenVisitor: Visitor {
    mutating func visit(_ node: Semantic) -> [any Markup] { node.content }

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

    // Label first, then content: they are two runs of one C child list, and
    // the dump's `children=` counts both.
    mutating func visit(_ node: DirectiveBlock) -> [any Markup] {
        (node.label.map { [$0 as any Markup] } ?? []) + node.content
    }

    mutating func visit(_ node: DirectiveLabel) -> [any Markup] { node.content }

    mutating func visit(_ node: FootnoteDefinition) -> [any Markup] { node.content }

    mutating func visit(_ node: ReferenceDefinition) -> [any Markup] { [] }

    mutating func visit(_ node: LinkReference) -> [any Markup] { node.content }

    mutating func visit(_ node: ImageReference) -> [any Markup] { node.content }

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

    mutating func visit(_ node: Directive) -> [any Markup] { node.label.map { [$0 as any Markup] } ?? [] }

    mutating func visit(_: FootnoteReference) -> [any Markup] { [] }

    mutating func visit(_ node: TableRow) -> [any Markup] { node.cells }

    mutating func visit(_ node: TableCell) -> [any Markup] { node.content }
}
