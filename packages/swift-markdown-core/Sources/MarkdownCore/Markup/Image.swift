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
    /// Required, for the reason ``Link/destination`` is.
    public let source: String
    /// Optional.
    public let title: String?

    /// Dispatches to the visitor's `Image` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer, content: [any Markup]) {
        var source = markdown_core_string()
        var title = markdown_core_optional_string()
        markdown_core_node_image_properties(node, &source, &title)
        self.init(
            scope: Self.scope(from: node),
            content: content,
            source: source.requiredString,
            title: title.string
        )
    }
}
