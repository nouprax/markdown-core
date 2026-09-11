import MarkdownCoreC

/// A definition-list association preserving its authored collections.
public struct Definition: Markup {
    /// The authored source range, including the term and all bodies.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The inline term, visited before the body collections.
    public let term: [any Markup]
    /// The nonempty ordered collection of block bodies; an individual body may be empty.
    public let content: [[any Markup]]
    /// Whether the first body immediately follows its term without a blank line.
    public let compact: Bool

    /// Dispatches to the visitor's `Definition` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Definition {
    init(from node: OpaquePointer, term: [any Markup], content: [[any Markup]]) {
        var compact = false
        precondition(markdown_core_node_definition_compact(node, &compact), "Invalid definition")
        precondition(!content.isEmpty, "Definition has no bodies")
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            term: term,
            content: content,
            compact: compact
        )
    }
}
