import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: MarkupReferences<any Markup>
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The paragraph's inline content.
    public var content: MarkupCollection<any Markup> { fields.children { $0.content } }
}

extension Paragraph.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: .init(indices: content)
        )
    }
}
