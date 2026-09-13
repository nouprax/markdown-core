import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The paragraph's inline content.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }

    /// Dispatches to the visitor's `Paragraph` case.
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
        guard case let .paragraph(fields) = tree.records[index] else {
            preconditionFailure("Invalid Paragraph record")
        }
        return fields
    }
}

extension Paragraph.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
