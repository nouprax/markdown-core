import MarkdownCoreC

public struct Link: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let destination: String?
    public let title: String?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer) {
        var destination = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_link_properties(node, &destination, &title)
        self.init(
            scope: Self.scope(from: node),
            children: Self.children(from: node),
            destination: destination.optionalString,
            title: title.optionalString
        )
    }
}
