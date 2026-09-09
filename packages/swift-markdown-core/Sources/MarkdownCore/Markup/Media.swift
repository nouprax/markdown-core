import MarkdownCoreC

/// Inline media — `![alt](source)` or a resolved reference.
/// The target type is not inferred.
///
/// Its content is PARSED alt text: `![a *b*](s)` has an ``Emphasis`` in it, and
/// flattening it to a string is the consumer's decision, not the parser's.
/// Complete `W`, `WxH`, `alt|W` and `alt|WxH` labels supply positive 32-bit
/// dimensions without leading zeros, on both direct and resolved images.
public struct Media: Markup {
    /// Where it is, `![` through the closing parenthesis. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// Parsed alt content excluding a valid dimension suffix; empty for a numeric-only label.
    /// A malformed suffix remains part of the alt content.
    public let content: [any Markup]
    /// Required, for the reason ``Link/dest`` is.
    public let dest: Destination
    /// Optional.
    public let title: String?
    /// Authored size from a complete label suffix, or nil. Independent of attribute records.
    public let dimensions: Dimensions?

    /// Dispatches to the visitor's `Media` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Media {
    init(from node: OpaquePointer, content: [any Markup], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        let dimensions = markdown_core_node_dimensions(node).map { Dimensions($0.pointee) }
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_attribute_value_anchor(markdown_core_node_primary_attributes(node)).string
                ?? resource.anchor,
            attributes: Attributes(from: node).inheriting(resource.attributes),
            content: content,
            dest: resource.dest,
            title: resource.title,
            dimensions: dimensions
        )
    }
}
