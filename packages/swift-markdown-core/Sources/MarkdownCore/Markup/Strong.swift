import MarkdownCoreC

/// Strongly emphasised text — two `*` or `_` pairs.
public struct Strong: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The emphasised inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Strong` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strong {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
