import MarkdownCoreC

public struct Link: Markup {
    public let scope: Scope
    public let content: [any Markup]
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `""`. A link with no destination at all is a
    /// ``LinkReference``.
    public let destination: String
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    public let title: String?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer) {
        var destination = markdown_core_string_view()
        var title = markdown_core_optional_string_view()
        markdown_core_node_link_properties(node, &destination, &title)
        self.init(
            scope: Self.scope(from: node),
            content: Self.children(from: node),
            destination: destination.requiredString,
            title: title.string
        )
    }
}
