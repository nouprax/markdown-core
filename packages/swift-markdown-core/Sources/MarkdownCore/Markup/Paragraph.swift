import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The paragraph's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Paragraph` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Paragraph {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
