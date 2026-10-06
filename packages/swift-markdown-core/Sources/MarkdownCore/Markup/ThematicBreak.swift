import MarkdownCoreC

/// A thematic break — a `***`, `---` or `___` line.
///
/// A leaf: it has no content, and its extent is all there is to read.
public struct ThematicBreak: Markup {
    let record: ThematicBreakRecord

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
}

final class ThematicBreakRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { ThematicBreak(record: self) }
}

extension ThematicBreakRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), children: [])
    }
}

extension ThematicBreak: RecordBacked {
    var base: MarkupRecord { record }
}
