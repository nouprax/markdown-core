import MarkdownCoreC

public struct Directive: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    public let mode: PlacementMode
    public let name: String
    public let attributes: String?
    public let labelCount: Int?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer, in decoder: NodeDecoder) {
        let (id, revision) = decoder.identity(of: node)
        let values = DirectiveValues(from: node)
        self.init(
            id: id,
            revision: revision,
            children: decoder.children(node),
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            labelCount: values.labelCount
        )
    }
}
