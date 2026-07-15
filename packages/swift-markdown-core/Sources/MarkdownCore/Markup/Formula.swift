import MarkdownCoreC

public struct Formula: Markup {
    public let scope: Scope
    public let children: [any Markup] = []
    public let mode: PlacementMode
    public let literal: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Formula {
    init(from node: OpaquePointer) {
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string_view()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            scope: Self.scope(from: node),
            mode: PlacementMode(from: mode),
            literal: literal.requiredString
        )
    }
}
