import MarkdownCoreC

/// A standalone formula. Requires the `formula` extension.
public struct FormulaBlock: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let literal: String
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The formula's body. Its delimiters or fence are in no literal.
    public var literal: String { fields.read { $0.literal } }
}

extension FormulaBlock.Fields {
    init(from node: OpaquePointer) {
        // A formula BLOCK is always standalone -- the engine's own
        // `markdown_core_elements_set_formula_mode` refuses any other value
        // for this kind -- so the mode is the kind and the model does not
        // repeat it. `Formula` is the one kind where it varies (Q29).
        var mode = MARKDOWN_CORE_PLACEMENT_STANDALONE
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            literal: literal.required
        )
    }
}
