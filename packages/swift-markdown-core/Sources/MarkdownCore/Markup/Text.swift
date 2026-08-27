import MarkdownCoreC

/// A run of literal text, with escapes and character references already resolved.
public struct Text: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The text as the reader sees it, not as the source spells it.
    public let literal: String

    /// Dispatches to the visitor's `Text` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Text {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
