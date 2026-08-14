import MarkdownCoreC

/// A hyperlink whose children are its inline caption.
public struct Link: Markup {
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
    /// The link destination, empty when the parentheses were written empty.
    ///
    /// Not optional: an inline link always writes its `(…)`, so there is no
    /// unwritten case to distinguish. `[a]()` and `[a](<>)` both give `""`.
    public let destination: String
    /// The title in quotes after the destination.
    ///
    /// Nil when none is written, the empty string when one is written empty:
    /// `[a](/u)` gives nil and `[a](/u "")` gives `""`. An autolink writes no
    /// title, so it gives nil like any other link.
    public let title: String?
    /// The link's inline caption content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var destination = markdown_core_string()
        var title = markdown_core_string()
        markdown_core_node_link_properties(node, &destination, &title)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            destination: destination.string,
            title: title.optional,
            content: builder.children(node)
        )
    }
}
