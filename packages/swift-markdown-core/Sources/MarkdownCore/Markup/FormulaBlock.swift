import MarkdownCoreC

public struct FormulaBlock: Markup {
    public let scope: Scope
    public let literal: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FormulaBlock {
    init(from node: OpaquePointer) {
        // A formula BLOCK is always standalone -- the engine's own
        // `markdown_core_extensions_set_formula_mode` refuses any other value
        // for this kind -- so the mode is the kind and the model does not
        // repeat it. `Formula` is the one kind where it varies (Q29).
        var mode = MARKDOWN_CORE_PLACEMENT_STANDALONE
        var literal = markdown_core_string_view()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
