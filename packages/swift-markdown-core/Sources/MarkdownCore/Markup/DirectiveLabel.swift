import MarkdownCoreC

/// A directive's bracketed label. Its scope spans the brackets, so a label
/// written empty is still a place in the source.
public struct DirectiveLabel: Markup {
    public let scope: Scope
    /// The label's inline content.
    public let content: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveLabel {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}
