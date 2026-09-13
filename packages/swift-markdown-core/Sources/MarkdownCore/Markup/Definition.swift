import MarkdownCoreC

/// A definition-list association preserving its authored collections.
public struct Definition: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let term: MarkupReferences<any Markup>
        let content: MarkupGroupReferences<any Markup>
        let compact: Bool
    }

    let fields: Stored<Fields>

    /// The authored source range, including the term and all bodies.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The inline term, visited before the body collections.
    public var term: MarkupCollection<any Markup> { fields.term }
    /// The nonempty ordered collection of block bodies; an individual body may be empty.
    public var content: MarkupGroups<any Markup> { fields.content }
    /// Whether the first body immediately follows its term without a blank line.
    public var compact: Bool { fields.compact }
}

extension Definition.Fields {
    init(from node: OpaquePointer, term: [Int], content: [[Int]]) {
        var compact = false
        precondition(markdown_core_node_definition_compact(node, &compact), "Invalid definition")
        precondition(!content.isEmpty, "Definition has no bodies")
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            term: .init(indices: term),
            content: .init(indices: content),
            compact: compact
        )
    }
}
