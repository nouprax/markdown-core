import MarkdownCoreC

/// A hyperlink whose children are its inline caption.
public struct Link: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// The link destination URL, if present.
    public let destination: String?
    /// The optional link title.
    public let title: String?

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var destination = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_link_properties(node, &destination, &title)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            destination: destination.optionalString,
            title: title.optionalString
        )
    }
}
