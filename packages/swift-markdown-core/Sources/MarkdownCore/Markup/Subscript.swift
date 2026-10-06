import MarkdownCoreC

/// Subscript inline content delimited by `~`.
public struct Subscript: Markup {
    let record: SubscriptRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class SubscriptRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Subscript(record: self) }
}

extension SubscriptRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Subscript: RecordBacked {
    var base: MarkupRecord { record }
}
