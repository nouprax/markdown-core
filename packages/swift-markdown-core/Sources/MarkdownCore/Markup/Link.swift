import MarkdownCoreC

public struct Link: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    public let destination: String?
    public let title: String?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer, in decoder: NodeDecoder) {
        let (id, revision) = decoder.identity(of: node)
        var destination = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_link_properties(node, &destination, &title)
        self.init(
            id: id,
            revision: revision,
            children: decoder.children(node),
            destination: destination.optionalString,
            title: title.optionalString
        )
    }
}
