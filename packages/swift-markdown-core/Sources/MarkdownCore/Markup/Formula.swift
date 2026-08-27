import MarkdownCoreC

/// A formula. Requires the `formula` extension.
///
/// The one kind that still carries ``PlacementMode``, because here it is a fact
/// about the source rather than about the kind.
public struct Formula: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// Whether the author wrote it inside a line or on its own.
    public let mode: PlacementMode
    /// The formula's body, its delimiters excluded. One leading and one
    /// trailing space or line ending is stripped when the body is not all
    /// whitespace.
    public let literal: String

    /// Dispatches to the visitor's `Formula` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Formula {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            mode: PlacementMode(from: mode),
            literal: literal.requiredString
        )
    }
}
