import MarkdownCoreC

/// A block of raw HTML, passed through unparsed.
public struct HTMLBlock: Markup {
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
    /// True when the literal is one complete comment, so consumers without
    /// an HTML parser can skip comment material by this bit alone.
    public let comment: Bool
    /// The raw HTML text.
    public let literal: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTMLBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            comment: node.htmlComment,
            literal: literal.string
        )
    }
}

extension OpaquePointer {
    /// The one-complete-comment bit, read from the parser rather than derived
    /// again here.
    ///
    /// The rule — after surrounding whitespace the literal opens with `<!--`
    /// and its first `-->` is the terminal bytes — belongs to the engine, and
    /// a projection that re-derives it is a second definition that can
    /// disagree with the first.
    var htmlComment: Bool {
        var comment = false
        markdown_core_node_html_comment(self, &comment)
        return comment
    }
}
