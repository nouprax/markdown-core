import MarkdownCoreC

/// An ATX or setext heading.
public struct Heading: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The document revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an
    /// append that only grows this node's extent leaves every reactive
    /// comparison untouched.
    public let scope: Scope
    /// The heading level, 1 through 6.
    public let level: Int32
    /// The heading's inline content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            level: level,
            content: builder.children(node)
        )
    }
}
