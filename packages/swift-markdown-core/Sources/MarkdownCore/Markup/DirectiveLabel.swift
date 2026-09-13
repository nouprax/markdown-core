import MarkdownCoreC

/// A directive's bracketed label. Its scope spans the brackets, so a label
/// written empty is still a place in the source.
public struct DirectiveLabel: Markup {
    /// Where it is, INCLUDING its brackets — which is what makes a label the
    /// source wrote empty still a place. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The label's inline content.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }

    /// Dispatches to the visitor's `DirectiveLabel` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
    }

    let tree: ValueTree
    let index: Int
    private var fields: Fields {
        guard case let .markupDirectiveLabel(fields) = tree.records[index] else {
            preconditionFailure("Invalid DirectiveLabel record")
        }
        return fields
    }
}

extension DirectiveLabel.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
