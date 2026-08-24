import MarkdownCoreC

/// A block quote.
public struct BlockQuote: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The quoted blocks. Block content, not inline.
    public let content: [any Markup]

    /// Dispatches to the visitor's `BlockQuote` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension BlockQuote {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
