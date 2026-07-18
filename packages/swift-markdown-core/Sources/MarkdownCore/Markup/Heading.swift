import MarkdownCoreC

public struct Heading: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    public let level: Int32

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.identity(of: node)
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(id: id, revision: revision, children: builder.children(node), level: level)
    }
}
