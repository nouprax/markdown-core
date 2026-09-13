import MarkdownCoreC

/// A callout — every `>` container.
///
/// A plain quoted block is a callout without metadata: `variant`, `collapsed`
/// and `title` are `nil`. A valid opening `[!type]` line
/// populates metadata; the type is stored as written.
public struct Callout: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let variant: String?
        let collapsed: Bool?
        let title: [Int]?
        let content: [Int]
    }

    @Stored var fields: Fields

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The authored type as written, or `nil` when the container has no
    /// metadata line.
    public var variant: String? { fields.variant }
    /// The fold marker: `nil` when no `+` or `-` was authored, `false` for
    /// `+`, which opens expanded, and `true` for `-`.
    public var collapsed: Bool? { fields.collapsed }
    /// The title's inline content, or `nil` when no title was authored; never
    /// empty. The callout owns it as a field; it is never part of `content`.
    public var title: MarkupCollection<any Markup>? {
        fields.title.map { $fields.collection($0) }
    }
    /// The quoted blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }

    /// Dispatches to the visitor's `Callout` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Callout.Fields {
    init(from node: OpaquePointer, title: [Int]?, content: [Int]) {
        var variant = markdown_core_optional_string()
        var collapsed = markdown_core_optional_bool()
        markdown_core_node_callout_properties(node, &variant, &collapsed)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            variant: variant.string,
            collapsed: collapsed.has_value ? collapsed.value : nil,
            title: title,
            content: content
        )
    }
}
