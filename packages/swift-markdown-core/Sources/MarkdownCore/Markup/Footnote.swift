import MarkdownCoreC

/// A footnote definition (`[^label]: …`); the owning session's footnote
/// queries answer numbering and back-references.
public struct FootnoteDefinition: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// The footnote's normalized label without the `[^` `]` delimiters.
    public let label: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var label = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &label)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            label: label.requiredString
        )
    }
}

/// A reference (`[^label]`) that resolves to a footnote definition.
public struct FootnoteReference: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Always empty: this node is a leaf.
    public let children: [any Markup] = []
    /// The footnote's normalized label without the `[^` `]` delimiters.
    public let label: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var label = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &label)
        self.init(id: id, revision: revision, label: label.requiredString)
    }
}
