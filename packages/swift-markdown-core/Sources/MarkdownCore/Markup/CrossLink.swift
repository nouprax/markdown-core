import MarkdownCoreC

/// An authored workspace reference, optionally requesting transclusion.
/// The raw label is absent only when no separator was written.
public struct CrossLink: Markup {
    /// The authored extent, including the optional embed marker and both bracket pairs.
    public let scope: Scope
    /// The declaration-side anchor, independent of the reference destination.
    public let anchor: String?
    /// Ordered attached classes and records.
    public let attributes: Attributes
    /// Whether the opener requested transclusion with an exclamation mark.
    public let embedded: Bool
    /// The raw workspace path and optional destination anchor.
    public let dest: Destination
    /// The raw authored label; nil when no separator was written.
    public let label: String?

    /// Dispatches to the visitor’s CrossLink case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CrossLink {
    init(from node: OpaquePointer) {
        var embedded = false
        var label = markdown_core_optional_string()
        markdown_core_node_cross_link_properties(node, &embedded, &label)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            embedded: embedded,
            dest: Destination(from: node),
            label: label.string
        )
    }
}
