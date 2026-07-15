import MarkdownCoreC

public struct Image: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let source: String?
    public let title: String?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer) {
        var source = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_image_properties(node, &source, &title)
        self.init(
            scope: Self.scope(from: node),
            children: Self.children(from: node),
            source: source.optionalString,
            title: title.optionalString
        )
    }
}
