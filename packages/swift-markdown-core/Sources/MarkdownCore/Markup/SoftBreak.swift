import MarkdownCoreC

/// A line ending inside a paragraph that the author did not force.
///
/// A leaf: it has no content, and its extent is all there is to read.
public struct SoftBreak: Markup {
    let record: SoftBreakRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class SoftBreakRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { SoftBreak(record: self) }
}

extension SoftBreakRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), children: [])
    }
}

extension SoftBreak: RecordBacked {
    var base: MarkupRecord { record }
}
