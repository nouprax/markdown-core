import MarkdownCoreC

/// An indented or fenced code block.
public struct CodeBlock: Markup {
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
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block.
    ///
    /// Always `standalone` for code blocks.
    public let mode: PlacementMode
    /// The full info string after the opening fence, if any.
    public let info: String?
    /// The first word of the info string — the conventional language tag.
    public let language: String?
    /// The code block's literal text.
    public let literal: String
    /// Whether the block was fenced rather than indented.
    public let fenced: Bool
    /// Whether a fenced block's closing fence was present.
    ///
    /// An unclosed fence runs to the end of the document; an indented block
    /// is closed by definition.
    public let closed: Bool

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CodeBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var info = markdown_core_string()
        var language = markdown_core_string()
        var literal = markdown_core_string()
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
        // The C facade fixes code blocks to standalone placement (the kind's
        // only legal mode), so no native call is needed.
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            mode: .standalone,
            info: info.optional,
            language: language.optional,
            literal: literal.string,
            fenced: fenced,
            closed: closed
        )
    }
}
