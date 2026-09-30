import MarkdownCoreC

/// A hard line break — a backslash or two or more spaces before the line ending.
///
/// A leaf: it has no content, and its extent is all there is to read.
public struct LineBreak: Markup {
    let record: LineBreakRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class LineBreakRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { LineBreak(record: self) }
}

extension LineBreakRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), children: [])
    }
}

extension LineBreak: RecordBacked {
    var base: MarkupRecord { record }
}
