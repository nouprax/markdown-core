import MarkdownCoreC

/// A workspace link written as `[[...]]`.
public struct CrossLink: Markup {
    let record: CrossLinkRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The full authored extent, including the delimiters.
    public var extent: Extent { record.extent }
    /// The declaration-side anchor, independent of the reference destination.
    public var anchor: String? { record.anchor }
    /// Ordered attached classes and records.
    public var attributes: Attributes { record.attributes }
    /// The raw workspace path and optional destination anchor.
    public var dest: Destination { record.dest }
    /// The raw authored label; nil when no separator was written.
    public var label: String? { record.label }
}

final class CrossLinkRecord: MarkupRecord, @unchecked Sendable {
    let dest: Destination
    let label: String?

    init(_ fields: InheritedFields, dest: Destination, label: String?) {
        self.dest = dest
        self.label = label
        super.init(fields, children: [])
    }

    override var markup: any Markup { CrossLink(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: CrossLinkRecord.self)
        return dest == other.dest && label == other.label
    }
}

extension CrossLinkRecord {
    convenience init(from node: OpaquePointer) {
        self.init(
            InheritedFields(from: node),
            dest: Destination(from: node),
            label: markdown_core_node_cross_label(node).string
        )
    }
}

extension CrossLink: RecordBacked {
    var base: MarkupRecord { record }
}
