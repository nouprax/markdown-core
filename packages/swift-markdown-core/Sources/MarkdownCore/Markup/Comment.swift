import MarkdownCoreC

/// A comment: an inline HTML comment token, or an HTML block that opened with
/// `<!--` and closed on a `-->` line. The one kind valid in both block and
/// inline content; the parent records which.
public struct Comment: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The bytes between the delimiters, exactly as written.
    public let literal: String

    /// Dispatches to the visitor's `Comment` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Comment {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            literal: literal.required
        )
    }
}
