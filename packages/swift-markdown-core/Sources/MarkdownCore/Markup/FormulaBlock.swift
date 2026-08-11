import MarkdownCoreC

/// A standalone formula block (the math extension).
public struct FormulaBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
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
        let track = builder.track(of: node)
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            mode: PlacementMode(from: mode),
            literal: literal.requiredString
        )
    }
}
