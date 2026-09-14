import MarkdownCoreC

/// A run of literal text, with escapes and character references already resolved.
public struct Text: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let literal: String
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The text as the reader sees it, not as the source spells it.
    public var literal: String { fields.read { $0.literal } }
}

extension Text.Fields {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            literal: literal.required
        )
    }
}
