import MarkdownCoreC

public struct SoftBreak: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup] = []

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension SoftBreak {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        self.init(id: id, revision: revision)
    }
}
