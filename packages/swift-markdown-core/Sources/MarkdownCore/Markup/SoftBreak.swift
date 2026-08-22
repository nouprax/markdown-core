import MarkdownCoreC

public struct SoftBreak: Markup {
    public let scope: Scope

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension SoftBreak {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node))
    }
}
