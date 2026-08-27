import MarkdownCoreC

/// A fenced or indented code block.
public struct CodeBlock: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The complete raw info string, or `nil` when the source wrote none. A
    /// fence with nothing but whitespace after it wrote none; an indented
    /// block has no fence to write one on.
    public let info: String?
    /// The info string's first whitespace-delimited token. Present exactly
    /// when ``info`` is.
    public let language: String?
    /// The block's content. Its fence and its indentation are in no literal.
    public let literal: String
    /// Whether the author fenced it. An indented block is `false`.
    public let fenced: Bool
    /// Whether a fenced block was closed before the document or its container
    /// ended. An indented block is always `true`, having nothing to close.
    public let closed: Bool

    /// Dispatches to the visitor's `CodeBlock` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CodeBlock {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        var info = markdown_core_optional_string()
        var language = markdown_core_optional_string()
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
        self.init(
            id: id,
            scope: Self.scope(from: node),
            info: info.string,
            language: language.string,
            literal: literal.requiredString,
            fenced: fenced,
            closed: closed
        )
    }
}
