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
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// Parsed alt content excluding a valid dimension suffix; empty for a numeric-only label.
    /// A malformed suffix remains part of the alt content.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }
    /// Required, for the reason ``Link/dest`` is.
    public var dest: Destination { fields.dest }
    /// Optional.
    public var title: String? { fields.title }
    /// Authored size from a complete label suffix, or nil. Independent of attribute records.
    public var dimensions: Dimensions? { fields.dimensions }

    /// Dispatches to the visitor's `Media` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
        let dest: Destination
        let title: String?
        let dimensions: Dimensions?
    }

    let tree: ValueTree
    let index: Int
    private var fields: Fields {
        guard case let .markupMedia(fields) = tree.records[index] else {
            preconditionFailure("Invalid Media record")
        }
        return fields
    }
}

extension Media.Fields {
    init(from node: OpaquePointer, content: [Int], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        let dimensions = markdown_core_node_dimensions(node).map { Dimensions($0.pointee) }
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
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
