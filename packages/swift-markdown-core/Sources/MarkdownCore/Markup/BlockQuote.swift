import MarkdownCoreC

/// A block quote.
public struct BlockQuote: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The quoted blocks. Block content, not inline.
    public let content: [any Markup]

    /// Dispatches to the visitor's `BlockQuote` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension BlockQuote {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        self.init(id: id, scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
