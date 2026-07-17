import MarkdownCoreC

public struct FormulaBlock: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup] = []
    public let mode: PlacementMode
    public let literal: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FormulaBlock {
    init(from node: OpaquePointer, in decoder: NodeDecoder) {
        let (id, revision) = decoder.identity(of: node)
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string_view()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            id: id,
            revision: revision,
            mode: PlacementMode(from: mode),
            literal: literal.requiredString
        )
    }
}
