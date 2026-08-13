import MarkdownCoreC

/// An image whose children are its inline description.
public struct Image: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The document revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// The image source, empty when the parentheses were written empty.
    ///
    /// Not optional: an inline image always writes its `(…)`, so there is no
    /// unwritten case to distinguish. `![a]()` gives `""`.
    public let source: String
    /// The optional image title.
    public let title: String?
    /// The image's parsed alt-text inline content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var source = markdown_core_string()
        var title = markdown_core_string()
        markdown_core_node_image_properties(node, &source, &title)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            source: source.string,
            title: title.optional,
            content: builder.children(node)
        )
    }
}
