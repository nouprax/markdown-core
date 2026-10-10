import MarkdownCoreC

/// Emphasised text — one `*` or `_` pair.
public struct Emphasis: Markup {
    let record: EmphasisRecord

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

final class EmphasisRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Emphasis(record: self) }
}

extension EmphasisRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Emphasis: RecordBacked {
    var base: MarkupRecord { record }
}
