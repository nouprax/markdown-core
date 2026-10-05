import MarkdownCoreC

/// An ordered, nonempty list of term/body associations.
public struct DefinitionList: Markup {
    let record: DefinitionListRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The authored source range, including the term and all bodies.
    public var extent: Extent { record.extent }
    /// The parts of its range that are its own, one per line. See ``Piece``.
    public var pieces: [Piece] { record.pieces }
    /// Where its first relation's content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The nonempty ordered collection of term/body associations.
    public var definitions: MarkupCollection<Definition> { record.collection(record.children.indices) }
}

final class DefinitionListRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { DefinitionList(record: self) }
}

extension DefinitionListRecord {
    convenience init(from node: OpaquePointer, definitions: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: definitions)
    }
}

extension DefinitionList: RecordBacked {
    var base: MarkupRecord { record }
}
