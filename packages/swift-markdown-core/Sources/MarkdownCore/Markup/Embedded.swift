import MarkdownCoreC

/// An inline embed — `![alt](source)` or a resolved reference.
/// The target type is not inferred.
///
/// Its content is PARSED alt text: `![a *b*](s)` has an ``Emphasis`` in it, and
/// flattening it to a string is the consumer's decision, not the parser's.
/// Complete `W`, `WxH`, `alt|W` and `alt|WxH` labels supply positive 32-bit
/// dimensions without leading zeros, on both direct and resolved images.
public struct Embedded: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: MarkupReferences<any Markup>
        let dest: Destination
        let title: String?
        let dimensions: Dimensions?
    }

    let fields: Stored<Fields>

    /// Where it is, `![` through the closing parenthesis. See ``Scope``.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// Parsed alt content excluding a valid dimension suffix; empty for a numeric-only label.
    /// A malformed suffix remains part of the alt content.
    public var content: MarkupCollection<any Markup> { fields.children { $0.content } }
    /// Required, for the reason ``Link/dest`` is.
    public var dest: Destination { fields.read { $0.dest } }
    /// Optional.
    public var title: String? { fields.read { $0.title } }
    /// Authored size from a complete label suffix, or nil. Independent of attribute records.
    public var dimensions: Dimensions? { fields.read { $0.dimensions } }
}

extension Embedded.Fields {
    init(from node: OpaquePointer, content: [Int], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        let dimensions = markdown_core_node_dimensions(node).map { Dimensions($0.pointee) }
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_attribute_value_anchor(markdown_core_node_primary_attributes(node)).string
                ?? resource.anchor,
            attributes: Attributes(from: node).inheriting(resource.attributes),
            content: .init(indices: content),
            dest: resource.dest,
            title: resource.title,
            dimensions: dimensions
        )
    }
}
