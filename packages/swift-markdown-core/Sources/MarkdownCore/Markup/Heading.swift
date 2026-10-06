import MarkdownCoreC

/// An ATX or setext heading.
///
/// Both spellings produce this one kind, and the node does not record which the
/// author used: `# Title` and `Title` over `=====` are the same heading.
public struct Heading: Markup {
    let record: HeadingRecord

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
    /// The heading's inline content, its `#` markers excluded.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// 1 through 6. A `#######` line is not a heading at all.
    public var level: Int32 { record.level }
}

final class HeadingRecord: MarkupRecord, @unchecked Sendable {
    let level: Int32

    init(_ fields: InheritedFields, level: Int32, content: [MarkupRecord]) {
        self.level = level
        super.init(fields, children: content)
    }

    override var markup: any Markup { Heading(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        level == unsafeDowncast(other, to: HeadingRecord.self).level
    }
}

extension HeadingRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(
            InheritedFields(from: node),
            level: answer(Int32(0)) { markdown_core_node_heading_level(node, $0) },
            content: content
        )
    }
}

extension Heading: RecordBacked {
    var base: MarkupRecord { record }
}
