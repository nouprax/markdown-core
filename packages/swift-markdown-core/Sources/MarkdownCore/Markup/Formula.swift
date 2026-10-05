import MarkdownCoreC

/// A formula. Requires the `formula` extension.
///
/// The one kind that still carries ``Placement``, because here it is a fact
/// about the source rather than about the kind.
public struct Formula: Markup {
    let record: FormulaRecord

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
    /// Whether the author wrote it inside a line or on its own.
    public var mode: Placement { record.mode }
    /// The formula's body, its delimiters excluded. One leading and one
    /// trailing space or line ending is stripped when the body is not all
    /// whitespace.
    public var literal: String { record.literal }
}

final class FormulaRecord: MarkupRecord, @unchecked Sendable {
    let mode: Placement
    let literal: String

    init(_ fields: InheritedFields, mode: Placement, literal: String) {
        self.mode = mode
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { Formula(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: FormulaRecord.self)
        return mode == other.mode && literal == other.literal
    }
}

extension FormulaRecord {
    convenience init(from node: OpaquePointer) {
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string()
        answered(markdown_core_node_formula_properties(node, &mode, &literal))
        self.init(InheritedFields(from: node), mode: Placement(from: mode), literal: literal.required)
    }
}

extension Formula: RecordBacked {
    var base: MarkupRecord { record }
}
