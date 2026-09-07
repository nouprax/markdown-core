import MarkdownCoreC

/// An inline image — `![alt](source)`.
///
/// Its content is PARSED alt text: `![a *b*](s)` has an ``Emphasis`` in it, and
/// flattening it to a string is the consumer's decision, not the parser's.
public struct Image: Markup {
    /// Where it is, `![` through the closing parenthesis. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The alt text, as parsed inline content.
    public let content: [any Markup]
    /// Required, for the reason ``Link/dest`` is.
    public let dest: Destination
    /// Optional.
    public let title: String?
    /// Authored dimensions, absent until O9.
    public let width: Int?
    /// Authored height, absent until O9.
    public let height: Int?

    /// Dispatches to the visitor's `Image` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Image {
    init(from node: OpaquePointer, content: [any Markup], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        var width = markdown_core_optional_i64()
        var height = markdown_core_optional_i64()
        markdown_core_node_image_dimensions(node, &width, &height)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            dest: resource.dest,
            title: resource.title,
            width: width.has_value ? Int(width.value) : nil,
            height: height.has_value ? Int(height.value) : nil
        )
    }
}
