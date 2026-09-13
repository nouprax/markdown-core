import MarkdownCoreC

/// A link — `[text](destination)`, any of the three reference forms, or an
/// autolink.
///
/// A reference occurrence is the link its definition names: it answers the
/// definition's destination and title and keeps its own scope.
public struct Link: Markup {
    /// Where it is, brackets and parentheses included. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The link text, as inline content.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `.url("")`; a reference occurrence answers the
    /// destination its definition stated.
    public var dest: Destination { fields.dest }
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    public var title: String? { fields.title }

    /// Dispatches to the visitor's `Link` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
        let dest: Destination
        let title: String?
    }

    @Stored var fields: Fields
}

extension Link.Fields {
    init(from node: OpaquePointer, content: [Int], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_attribute_value_anchor(markdown_core_node_primary_attributes(node)).string
                ?? resource.anchor,
            attributes: Attributes(from: node).inheriting(resource.attributes),
            content: content,
            dest: resource.dest,
            title: resource.title
        )
    }
}
