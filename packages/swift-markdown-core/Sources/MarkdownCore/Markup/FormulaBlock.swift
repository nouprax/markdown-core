import MarkdownCoreC

/// A standalone formula. Requires the `formula` extension.
public struct FormulaBlock: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The formula's body. Its delimiters or fence are in no literal.
    public let literal: String

    /// Dispatches to the visitor's `FormulaBlock` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FormulaBlock {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        // A formula BLOCK is always standalone -- the engine's own
        // `markdown_core_extensions_set_formula_mode` refuses any other value
        // for this kind -- so the mode is the kind and the model does not
        // repeat it. `Formula` is the one kind where it varies (Q29).
        var mode = MARKDOWN_CORE_PLACEMENT_STANDALONE
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(id: id, scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
