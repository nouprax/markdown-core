import MarkdownCoreC

/// A directive's bracketed label. Its scope spans the brackets, so a label
/// written empty is still a place in the source.
public struct DirectiveLabel: Markup {
    /// Where it is, INCLUDING its brackets — which is what makes a label the
    /// source wrote empty still a place. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The label's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `DirectiveLabel` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveLabel {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
