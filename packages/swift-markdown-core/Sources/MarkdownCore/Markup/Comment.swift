import MarkdownCoreC

/// A comment: an inline HTML comment token, or an HTML block that opened with
/// `<!--` and closed on a `-->` line. The one kind valid in both block and
/// inline content; the parent records which.
public struct Comment: Markup {
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
    /// The bytes between the delimiters, exactly as written.
    public var literal: String { fields.read { $0.literal } }
}

extension Comment.Fields {
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
