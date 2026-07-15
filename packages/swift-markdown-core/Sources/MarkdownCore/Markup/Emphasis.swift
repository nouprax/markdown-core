import MarkdownCoreC

public struct Emphasis: Markup {
    public let scope: Scope
    public let children: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Emphasis {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), children: Self.children(from: node))
    }
}
