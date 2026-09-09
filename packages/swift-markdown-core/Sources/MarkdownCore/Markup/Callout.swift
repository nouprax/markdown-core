import MarkdownCoreC

/// A callout — every `>` container.
///
/// A plain quoted block is a callout without metadata: `variant`, `collapsed`
/// and `title` are `nil`. A valid opening `[!type]` line
/// populates metadata; the type is stored as written.
public struct Callout: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The authored type as written, or `nil` when the container has no
    /// metadata line.
    public let variant: String?
    /// The fold marker: `nil` when no `+` or `-` was authored, `false` for
    /// `+`, which opens expanded, and `true` for `-`.
    public let collapsed: Bool?
    /// The title's inline content, or `nil` when no title was authored; never
    /// empty. The callout owns it as a field; it is never part of `content`.
    public let title: [any Markup]?
    /// The quoted blocks. Block content, not inline.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Callout` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Callout {
    init(from node: OpaquePointer, title: [any Markup]?, content: [any Markup]) {
        var variant = markdown_core_optional_string()
        var collapsed = markdown_core_optional_bool()
        markdown_core_node_callout_properties(node, &variant, &collapsed)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            variant: variant.string,
            collapsed: collapsed.has_value ? collapsed.value : nil,
            title: title,
            content: content
        )
    }
}
