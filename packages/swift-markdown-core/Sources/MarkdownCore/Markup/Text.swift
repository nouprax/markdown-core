import MarkdownCoreC

/// A run of literal text, with escapes and character references already resolved.
public struct Text: Markup {
    let record: TextRecord

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
    /// The text as the reader sees it, not as the source spells it.
    public var literal: String { record.literal }
}

final class TextRecord: MarkupRecord, @unchecked Sendable {
    let literal: String

    init(_ fields: InheritedFields, literal: String) {
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { Text(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        literal == unsafeDowncast(other, to: TextRecord.self).literal
    }
}

extension TextRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), literal: nativeLiteral(of: node))
    }
}

extension Text: RecordBacked {
    var base: MarkupRecord { record }
}
