import MarkdownCoreC

/// An ATX or setext heading.
///
/// Both spellings produce this one kind, and the node does not record which the
/// author used: `# Title` and `Title` over `=====` are the same heading.
public struct Heading: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The heading's inline content, its `#` markers excluded.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }
    /// 1 through 6. A `#######` line is not a heading at all.
    public var level: Int32 { fields.level }

    /// Dispatches to the visitor's `Heading` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
        let level: Int32
    }

    let tree: ValueTree
    let index: Int
    private var fields: Fields {
        guard case let .markupHeading(fields) = tree.records[index] else {
            preconditionFailure("Invalid Heading record")
        }
        return fields
    }
}

extension Heading.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            level: level
        )
    }
}
