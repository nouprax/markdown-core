import MarkdownCoreC

/// A directive's bracketed label. Its extent spans the brackets, so a label
/// written empty is still a place in the source.
public struct DirectiveLabel: Markup {
    let record: DirectiveLabelRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, INCLUDING its brackets — which is what makes a label the
    /// source wrote empty still a place. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The label's inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class DirectiveLabelRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { DirectiveLabel(record: self) }
}

extension DirectiveLabelRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension DirectiveLabel: RecordBacked {
    var base: MarkupRecord { record }
}
