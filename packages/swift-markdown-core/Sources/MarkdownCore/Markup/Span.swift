import MarkdownCoreC

/// Inline content with an authored attribute container.
public struct Span: Markup {
    let record: SpanRecord

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

final class SpanRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Span(record: self) }
}

extension SpanRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Span: RecordBacked {
    var base: MarkupRecord { record }
}
