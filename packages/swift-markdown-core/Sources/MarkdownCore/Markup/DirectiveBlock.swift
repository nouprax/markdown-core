import MarkdownCoreC

/// A named leaf or container directive, or a nameless fenced container.
///
/// Nameless containers use `::: {.class}` or `::: class` and have a nil name.
/// All container forms share the same closing-fence and block-content rules.
public struct DirectiveBlock: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let name: String?
        let label: MarkupReference<DirectiveLabel>?
        let content: MarkupReferences<any Markup>
    }

    let fields: Stored<Fields>

    /// Where it is, opening fence through closing fence. See ``Scope``.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The directive's name without colons, or nil for a nameless container.
    public var name: String? { fields.read { $0.name } }
    /// The bracketed label, or `nil` when the source wrote none.
    public var label: DirectiveLabel? { fields.optionalElement { $0.label } }
    /// The block content the fence encloses.
    public var content: MarkupCollection<any Markup> { fields.children { $0.content } }
}

extension DirectiveBlock.Fields {
    init(from node: OpaquePointer, label: Int?, content: [Int]) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            name: values.name,
            label: label.map { .init(index: $0) },
            content: .init(indices: content)
        )
    }
}
