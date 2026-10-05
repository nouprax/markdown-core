import MarkdownCoreC

/// Superscript inline content delimited by `^`.
public struct Superscript: Markup {
    let record: SuperscriptRecord

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
    /// The inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class SuperscriptRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Superscript(record: self) }
}

extension SuperscriptRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension Superscript: RecordBacked {
    var base: MarkupRecord { record }
}
