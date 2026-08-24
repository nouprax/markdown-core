import MarkdownCoreC

/// Emphasised text — one `*` or `_` pair.
public struct Emphasis: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The emphasised inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Emphasis` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Emphasis {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
