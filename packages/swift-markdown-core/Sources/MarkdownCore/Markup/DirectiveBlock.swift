import MarkdownCoreC

/// A named leaf or container directive, or a nameless fenced container.
///
/// Nameless containers use `::: {.class}` or `::: class` and have a nil name.
/// All container forms share the same closing-fence and block-content rules.
public struct DirectiveBlock: Markup {
    /// Where it is, opening fence through closing fence. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The directive's name without colons, or nil for a nameless container.
    public var name: String? { fields.name }
    /// The bracketed label, or `nil` when the source wrote none.
    public var label: DirectiveLabel? { fields.label.map { tree.value(at: $0, as: DirectiveLabel.self) } }
    /// The block content the fence encloses.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }

    /// Dispatches to the visitor's `DirectiveBlock` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let name: String?
        let label: Int?
        let content: [Int]
    }

    let tree: ValueTree
    let index: Int
    private var fields: Fields {
        guard case let .directiveBlock(fields) = tree.records[index] else {
            preconditionFailure("Invalid DirectiveBlock record")
        }
        return fields
    }
}

extension DirectiveBlock.Fields {
    init(from node: OpaquePointer, label: Int?, content: [Int]) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            name: values.name,
            label: label,
            content: content
        )
    }
}
