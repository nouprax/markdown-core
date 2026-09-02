import MarkdownCoreC

/// Strongly emphasised text — two `*` or `_` pairs.
public struct Strong: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The emphasised inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Strong` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strong {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(scope: Self.scope(from: node), content: content)
    }
}
