import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    let record: ParagraphRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The parts of its range that are its own, one per line. See ``Piece``.
    public var pieces: [Piece] { record.pieces }
    /// Where its first relation's content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The paragraph's inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class ParagraphRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Paragraph(record: self) }
}

extension ParagraphRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Paragraph: RecordBacked {
    var base: MarkupRecord { record }
}
