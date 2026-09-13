import MarkdownCoreC

/// Highlighted inline content delimited by `==`.
public struct Mark: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
    }

    @Stored var fields: Fields

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The highlighted inline content.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }

    /// Dispatches to the visitor's `Mark` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Mark.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content
        )
    }
}
