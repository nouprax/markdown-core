import MarkdownCoreC

/// A run of literal inline text.
public struct Text: Markup {
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
    /// The decoded text content.
    public let literal: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Text {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(id: track.id, revision: track.revision, scope: track.scope, literal: literal.string)
    }
}
