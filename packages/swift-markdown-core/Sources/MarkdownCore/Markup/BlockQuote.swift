import MarkdownCoreC

public struct BlockQuote: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension BlockQuote {
    init(from node: OpaquePointer, in decoder: NodeDecoder) {
        let (id, revision) = decoder.identity(of: node)
        self.init(id: id, revision: revision, children: decoder.children(node))
    }
}
