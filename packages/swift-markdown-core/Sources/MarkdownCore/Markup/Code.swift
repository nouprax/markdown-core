import MarkdownCoreC

/// An inline code span.
public struct Code: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The span's content. Its backticks are in no literal anywhere.
    public let literal: String

    /// Dispatches to the visitor's `Code` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Code {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
