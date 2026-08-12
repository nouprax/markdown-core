import MarkdownCoreC

/// A footnote definition (`[^label]: …`), at the position it was written.
///
/// - Never moved to the document tail.
/// - Never dropped when nothing references it.
/// - Never reordered by use.
///
/// It carries no number, because a number is a rendering decision and the
/// tree does not make one.
public struct FootnoteDefinition: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// The label between `[^` and `]`, exactly as written and not normalized.
    ///
    /// A reference and a definition are paired case-folded, trimmed, and with
    /// inner whitespace collapsed, so comparing two of these strings byte for
    /// byte is a stricter test than the one that matched them.
    public let label: String
    /// The definition's block content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var label = markdown_core_string()
        markdown_core_node_footnote_id(node, &label)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            label: label.string,
            content: builder.children(node)
        )
    }
}

/// A reference (`[^label]`) that resolves to a footnote definition.
///
/// A reference with no definition is not one: it stays the literal text the
/// author typed, and that text is not reparsed — `[^~~x~~]` with nothing
/// defining it is one ``Text`` holding no ``Strikethrough``. A consumer
/// never meets an unresolvable reference node.
public struct FootnoteReference: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// The label between `[^` and `]`, exactly as written and not normalized.
    ///
    /// A reference and a definition are paired case-folded, trimmed, and with
    /// inner whitespace collapsed, so comparing two of these strings byte for
    /// byte is a stricter test than the one that matched them.
    public let label: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var label = markdown_core_string()
        markdown_core_node_footnote_id(node, &label)
        self.init(id: track.id, revision: track.revision, scope: track.scope, label: label.string)
    }
}
