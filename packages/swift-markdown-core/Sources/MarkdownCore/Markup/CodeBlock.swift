import MarkdownCoreC

/// A fenced or indented code block.
public struct CodeBlock: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let info: String?
        let language: String?
        let literal: String
        let fenced: Bool
        let closed: Bool
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The complete raw info string, or `nil` when the source wrote none. A
    /// fence with nothing but whitespace after it wrote none; an indented
    /// block has no fence to write one on.
    public var info: String? { fields.read { $0.info } }
    /// The info string's first whitespace-delimited token. Present exactly
    /// when ``info`` is.
    public var language: String? { fields.read { $0.language } }
    /// The block's content. Its fence and its indentation are in no literal.
    public var literal: String { fields.read { $0.literal } }
    /// Whether the author fenced it. An indented block is `false`.
    public var fenced: Bool { fields.read { $0.fenced } }
    /// Whether a fenced block was closed before the document or its container
    /// ended. An indented block is always `true`, having nothing to close.
    public var closed: Bool { fields.read { $0.closed } }
}

extension CodeBlock.Fields {
    init(from node: OpaquePointer) {
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
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            info: info.string,
            language: language.string,
            literal: literal.required,
            fenced: fenced,
            closed: closed
        )
    }
}
