import MarkdownCoreC

/// An inline code span.
public struct Code: Markup {
    let record: CodeRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The span's content. Its backticks are in no literal anywhere.
    public var literal: String { record.literal }
}

final class CodeRecord: MarkupRecord, @unchecked Sendable {
    let literal: String

    init(_ fields: InheritedFields, literal: String) {
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { Code(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        literal == unsafeDowncast(other, to: CodeRecord.self).literal
    }
}

extension CodeRecord {
    convenience init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(InheritedFields(from: node), literal: literal.required)
    }
}

extension Code: RecordBacked {
    var base: MarkupRecord { record }
}
