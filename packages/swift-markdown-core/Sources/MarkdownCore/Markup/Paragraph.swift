import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The paragraph's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Paragraph` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Paragraph {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
