import MarkdownCoreC

/// An inline image — `![alt](source)`.
///
/// Its content is PARSED alt text: `![a *b*](s)` has an ``Emphasis`` in it, and
/// flattening it to a string is the consumer's decision, not the parser's.
public struct Image: Markup {
    /// Where it is, `![` through the closing parenthesis. See ``Scope``.
    public let scope: Scope
    /// The alt text, as parsed inline content.
    public let content: [any Markup]
    /// Required, for the reason ``Link/dest`` is.
    public let dest: Destination
    /// Optional.
    public let title: String?

    /// Dispatches to the visitor's `Image` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer, content: [any Markup]) {
        var title = markdown_core_optional_string()
        markdown_core_node_title(node, &title)
        self.init(
            scope: Self.scope(from: node),
            content: content,
            dest: Destination(from: node),
            title: title.string
        )
    }
}
