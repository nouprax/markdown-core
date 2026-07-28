import MarkdownCoreC

/// An ATX or setext heading.
public struct Heading: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// The heading level, 1 through 6.
    public let level: Int32

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(id: id, revision: revision, children: builder.children(node), level: level)
    }
}
