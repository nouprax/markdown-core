import MarkdownCoreC

/// An inline code span.
public struct Code: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Always empty: this node is a leaf.
    public let children: [any Markup] = []
    /// The code span's literal text.
    public let literal: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Code {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, revision: revision, literal: literal.requiredString)
    }
}
