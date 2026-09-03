import MarkdownCoreC

/// Struck-through text. Requires the `strikethrough` extension.
public struct Strikethrough: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The struck-through inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Strikethrough` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strikethrough {
    init(from node: OpaquePointer, content: [any Markup]) {
        self.init(scope: Self.scope(from: node), content: content)
    }
}
