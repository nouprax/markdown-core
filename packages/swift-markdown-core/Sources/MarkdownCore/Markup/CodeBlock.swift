import MarkdownCoreC

/// An indented or fenced code block.
public struct CodeBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Always empty: this node is a leaf.
    public let children: [any Markup] = []
    /// The full info string after the opening fence, if any.
    public let info: String?
    /// The first word of the info string — the conventional language tag.
    public let language: String?
    /// The code block's literal text.
    public let literal: String
    /// Whether the block was fenced rather than indented.
    public let isFenced: Bool
    /// Whether a fenced block's closing fence was present; streaming input
    /// parsed mid-block reports `false`.
    public let isClosed: Bool

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CodeBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var info = markdown_core_string_view()
        var language = markdown_core_string_view()
        var literal = markdown_core_string_view()
        var fenced = false
        var closed = false
        markdown_core_node_code_block_properties(
            node,
            &info,
            &language,
            &literal,
            &fenced,
            &closed
        )
        self.init(
            id: id,
            revision: revision,
            info: info.optionalString,
            language: language.optionalString,
            literal: literal.requiredString,
            isFenced: fenced,
            isClosed: closed
        )
    }
}
