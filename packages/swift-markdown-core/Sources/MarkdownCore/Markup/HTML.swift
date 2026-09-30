import MarkdownCoreC

/// A run of raw inline HTML.
public struct HTML: Markup {
    let record: HTMLRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The HTML exactly as written. Nothing in it is parsed or escaped.
    public var literal: String { record.literal }
}

final class HTMLRecord: MarkupRecord, @unchecked Sendable {
    let literal: String

    init(_ fields: InheritedFields, literal: String) {
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { HTML(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        literal == unsafeDowncast(other, to: HTMLRecord.self).literal
    }
}

extension HTMLRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), literal: nativeLiteral(of: node))
    }
}

extension HTML: RecordBacked {
    var base: MarkupRecord { record }
}
