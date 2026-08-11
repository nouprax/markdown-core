import MarkdownCoreC

/// A block of raw HTML, passed through unparsed.
public struct HTMLBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
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
        let (id, revision) = builder.id(of: node)
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, revision: revision, comment: node.htmlComment, literal: literal.requiredString)
    }
}

extension OpaquePointer {
    /// The one-complete-comment bit, read from the parser rather than derived
    /// again here. The rule — after surrounding whitespace the literal opens
    /// with `<!--` and its first `-->` is the terminal bytes — belongs to the
    /// engine, and a projection that re-derives it is a second definition
    /// that can disagree with the first.
    var htmlComment: Bool {
        var comment = false
        markdown_core_node_html_comment(self, &comment)
        return comment
    }
}
