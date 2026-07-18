import MarkdownCoreC

public struct DirectiveBlock: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    public let mode: PlacementMode
    public let name: String
    public let attributes: String?
    public let labelCount: Int?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.identity(of: node)
        let values = DirectiveValues(from: node)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            labelCount: values.labelCount
        )
    }
}
