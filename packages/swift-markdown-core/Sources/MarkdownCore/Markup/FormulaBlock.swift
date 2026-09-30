import MarkdownCoreC

/// A standalone formula. Requires the `formula` extension.
public struct FormulaBlock: Markup {
    let record: FormulaBlockRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The formula's body. Its delimiters or fence are in no literal.
    public var literal: String { record.literal }
}

final class FormulaBlockRecord: MarkupRecord, @unchecked Sendable {
    let literal: String

    init(_ fields: InheritedFields, literal: String) {
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { FormulaBlock(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        literal == unsafeDowncast(other, to: FormulaBlockRecord.self).literal
    }
}

extension FormulaBlockRecord {
    convenience init(from node: OpaquePointer) {
        // A formula BLOCK is always standalone, so the mode is the kind and
        // the model does not repeat it. `Formula` is the one kind where it
        // varies (Q29).
        var mode = MARKDOWN_CORE_PLACEMENT_STANDALONE
        var literal = markdown_core_string()
        answered(markdown_core_node_formula_properties(node, &mode, &literal))
        self.init(InheritedFields(from: node), literal: literal.required)
    }
}

extension FormulaBlock: RecordBacked {
    var base: MarkupRecord { record }
}
