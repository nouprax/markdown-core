import MarkdownCoreC

/// A formula. Requires the `formula` extension.
///
/// The one kind that still carries ``Placement``, because here it is a fact
/// about the source rather than about the kind.
public struct Formula: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let mode: Placement
        let literal: String
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// Whether the author wrote it inside a line or on its own.
    public var mode: Placement { fields.read { $0.mode } }
    /// The formula's body, its delimiters excluded. One leading and one
    /// trailing space or line ending is stripped when the body is not all
    /// whitespace.
    public var literal: String { fields.read { $0.literal } }
}

extension Formula.Fields {
    init(from node: OpaquePointer) {
        var mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var literal = markdown_core_string()
        markdown_core_node_formula_properties(node, &mode, &literal)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            mode: Placement(from: mode),
            literal: literal.required
        )
    }
}
