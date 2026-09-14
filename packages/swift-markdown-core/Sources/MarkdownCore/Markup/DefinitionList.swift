import MarkdownCoreC

/// An ordered, nonempty list of term/body associations.
public struct DefinitionList: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let definitions: MarkupReferences<Definition>
    }

    let fields: Stored<Fields>

    /// The authored source range, including the term and all bodies.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The nonempty ordered collection of term/body associations.
    public var definitions: MarkupCollection<Definition> { fields.elements { $0.definitions } }
}

extension DefinitionList.Fields {
    init(from node: OpaquePointer, children: [Int]) {
        precondition(!children.isEmpty, "Empty definition list")
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            definitions: .init(indices: children)
        )
    }
}
