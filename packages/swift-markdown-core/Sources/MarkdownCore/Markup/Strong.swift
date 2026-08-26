import MarkdownCoreC

/// Strongly emphasised text — two `*` or `_` pairs.
public struct Strong: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The emphasised inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Strong` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strong {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
