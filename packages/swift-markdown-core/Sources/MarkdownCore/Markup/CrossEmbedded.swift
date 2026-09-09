import MarkdownCoreC

/// A workspace transclusion written as `![[...]]`.
public struct CrossEmbedded: Markup {
    /// The full authored extent, including the delimiters.
    public let scope: Scope
    /// The declaration-side anchor, independent of the reference destination.
    public let anchor: String?
    /// Ordered attached classes and records.
    public let attributes: Attributes
    /// The raw workspace path and optional destination anchor.
    public let dest: Destination
    /// Raw prefix after a valid dimension suffix; nil when no separator was written.
    public let label: String?
    /// Authored size, absent when no complete valid suffix was recognized.
    public let dimensions: Dimensions?

    /// Dispatches to the visitor's CrossEmbedded case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CrossEmbedded {
    init(from node: OpaquePointer) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            dest: Destination(from: node),
            label: markdown_core_node_cross_label(node).string,
            dimensions: markdown_core_node_dimensions(node).map { Dimensions($0.pointee) }
        )
    }
}
