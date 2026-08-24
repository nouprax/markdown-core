import MarkdownCoreC

/// A run of literal text, with escapes and character references already resolved.
public struct Text: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The text as the reader sees it, not as the source spells it.
    public let literal: String

    /// Dispatches to the visitor's `Text` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Text {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
