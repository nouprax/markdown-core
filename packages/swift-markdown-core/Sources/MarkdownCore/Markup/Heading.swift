import MarkdownCoreC

/// An ATX or setext heading.
public struct Heading: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The heading level, 1 through 6.
    public let level: Int32
    /// The heading's inline content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(id: id, revision: revision, level: level, content: builder.children(node))
    }
}
