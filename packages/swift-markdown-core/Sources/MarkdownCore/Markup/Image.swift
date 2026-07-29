import MarkdownCoreC

/// An image whose children are its inline description.
public struct Image: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The image source URL, if present.
    public let source: String?
    /// The optional image title.
    public let title: String?
    /// The image's parsed alt-text inline content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var source = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_image_properties(node, &source, &title)
        self.init(
            id: id,
            revision: revision,
            source: source.optionalString,
            title: title.optionalString,
            content: builder.children(node)
        )
    }
}
