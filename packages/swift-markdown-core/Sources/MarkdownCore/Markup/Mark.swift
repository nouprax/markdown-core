import MarkdownCoreC

/// Highlighted inline content delimited by `==`.
public struct Mark: Markup {
    let record: MarkRecord

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
    /// The highlighted inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class MarkRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Mark(record: self) }
}

extension MarkRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Mark: RecordBacked {
    var base: MarkupRecord { record }
}
