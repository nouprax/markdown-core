import MarkdownCoreC

/// A `#name` topic: an inline leaf whose name excludes the `#`.
public struct Topic: Markup {
    let record: TopicRecord

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
    /// The name as written, without the `#`.
    public var name: String { record.name }
}

final class TopicRecord: MarkupRecord, @unchecked Sendable {
    let name: String

    init(_ fields: InheritedFields, name: String) {
        self.name = name
        super.init(fields, children: [])
    }

    override var markup: any Markup { Topic(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        name == unsafeDowncast(other, to: TopicRecord.self).name
    }
}

extension TopicRecord {
    convenience init(from topic: OpaquePointer) {
        self.init(
            InheritedFields(from: topic),
            name: answer(markdown_core_string()) { markdown_core_topic_name(topic, $0) }.required
        )
    }
}

extension Topic: RecordBacked {
    var base: MarkupRecord { record }
}
