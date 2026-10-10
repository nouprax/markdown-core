import MarkdownCoreC

/// Strongly emphasised text — two `*` or `_` pairs.
public struct Strong: Markup {
    let record: StrongRecord

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
    /// The emphasised inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class StrongRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Strong(record: self) }
}

extension StrongRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Strong: RecordBacked {
    var base: MarkupRecord { record }
}
