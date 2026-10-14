import MarkdownCoreC

/// A fenced or indented code block.
public struct CodeBlock: Markup {
    let record: CodeBlockRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The complete raw info string, or `nil` when the source wrote none. A
    /// fence with nothing but whitespace after it wrote none; an indented
    /// block has no fence to write one on.
    public var info: String? { record.info }
    /// The info string's first whitespace-delimited token. Present exactly
    /// when ``info`` is.
    public var language: String? { record.language }
    /// The block's content. Its fence and its indentation are in no literal.
    public var literal: String { record.literal }
    /// Whether the author fenced it. An indented block is `false`.
    public var fenced: Bool { record.fenced }
    /// Whether a fenced block was closed before the document or its container
    /// ended. An indented block is always `true`, having nothing to close.
    public var closed: Bool { record.closed }
}

final class CodeBlockRecord: MarkupRecord, @unchecked Sendable {
    let info: String?
    let language: String?
    let literal: String
    let fenced: Bool
    let closed: Bool

    init(_ fields: InheritedFields, info: String?, language: String?, literal: String, fenced: Bool, closed: Bool) {
        self.info = info
        self.language = language
        self.literal = literal
        self.fenced = fenced
        self.closed = closed
        super.init(fields, children: [])
    }

    override var markup: any Markup { CodeBlock(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: CodeBlockRecord.self)
        return info == other.info && language == other.language && literal == other.literal
            && fenced == other.fenced && closed == other.closed
    }
}

extension CodeBlockRecord {
    convenience init(from node: OpaquePointer) {
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
            InheritedFields(from: node),
            info: info.string,
            language: language.string,
            literal: literal.required,
            fenced: fenced,
            closed: closed
        )
    }
}

extension CodeBlock: RecordBacked {
    var base: MarkupRecord { record }
}
