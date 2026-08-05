import MarkdownCoreC

/// A run of raw inline HTML, passed through unparsed.
public struct HTML: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The raw HTML text.
    public let literal: String

    /// True when the literal is one complete comment; the same rule as
    /// `HTMLBlock.comment`.
    public var comment: Bool { htmlLiteralIsComment(literal) }

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTML {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, revision: revision, literal: literal.requiredString)
    }
}
