import MarkdownCoreC

/// An inline formula (the math extension).
public struct Formula: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
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
    /// `standalone` for a display formula, `embedded` for one that runs with
    /// the surrounding text.
    ///
    /// Either way the node is inline content, and ``FormulaBlock`` is the
    /// kind for a formula that occupies a block of its own.
    public let mode: PlacementMode
    /// The formula source between the delimiters.
    public let literal: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Formula {
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
            literal: literal.string
        )
    }
}
