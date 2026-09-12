import MarkdownCoreC

/// A standalone formula. Requires the `formula` extension.
public struct FormulaBlock: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The formula's body. Its delimiters or fence are in no literal.
    public let literal: String

    /// Dispatches to the visitor's `FormulaBlock` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FormulaBlock {
    init(from node: OpaquePointer) {
        // A formula BLOCK is always standalone -- the engine's own
        // `markdown_core_elements_set_formula_mode` refuses any other value
        // for this kind -- so the mode is the kind and the model does not
        // repeat it. `Formula` is the one kind where it varies (Q29).
        var mode = MARKDOWN_CORE_PLACEMENT_STANDALONE
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            literal: literal.requiredString
        )
    }
}
