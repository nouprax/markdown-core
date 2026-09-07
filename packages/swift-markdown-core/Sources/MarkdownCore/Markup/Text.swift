import MarkdownCoreC

/// A run of literal text, with escapes and character references already resolved.
public struct Text: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The text as the reader sees it, not as the source spells it.
    public let literal: String

    /// Dispatches to the visitor's `Text` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Text {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            literal: literal.requiredString
        )
    }
}
