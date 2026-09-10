import MarkdownCoreC

/// Inline content with an authored attribute container.
public struct Span: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Span` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Span {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
