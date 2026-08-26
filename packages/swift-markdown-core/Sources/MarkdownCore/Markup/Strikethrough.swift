import MarkdownCoreC

/// Struck-through text. Requires the `strikethrough` extension.
public struct Strikethrough: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The struck-through inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Strikethrough` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strikethrough {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
