import MarkdownCoreC

/// A named leaf or container directive, or a nameless fenced container.
///
/// Nameless containers use `::: {.class}` or `::: class` and have a nil name.
/// All container forms share the same closing-fence and block-content rules.
public struct DirectiveBlock: Markup {
    /// Where it is, opening fence through closing fence. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The directive's name without colons, or nil for a nameless container.
    public let name: String?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?
    /// The block content the fence encloses.
    public let content: [any Markup]

    /// Dispatches to the visitor's `DirectiveBlock` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer, label: DirectiveLabel?, content: [any Markup]) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            name: values.name,
            label: label,
            content: content
        )
    }
}
