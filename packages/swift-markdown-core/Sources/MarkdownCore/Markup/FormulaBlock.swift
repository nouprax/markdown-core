import MarkdownCoreC

/// A standalone formula block (the math extension).
public struct FormulaBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Always empty: this node is a leaf.
    public let children: [any Markup] = []
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block.
    public let mode: PlacementMode
    /// The formula source between the delimiters.
    public let literal: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FormulaBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
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
