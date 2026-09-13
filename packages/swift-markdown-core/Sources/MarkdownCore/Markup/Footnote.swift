import MarkdownCoreC

/// A footnote the document owns: a Markup node,
/// reached through ``Document/footnotes`` and never an element of any content
/// list.
///
/// Repeated calls share one footnote, the first definition of an id wins, and
/// a valid definition nobody calls is still a footnote. The walk reports it
/// through the ``MarkupVisitor`` case that takes a `Footnote`, after
/// the document's content.
public struct Footnote: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let id: String
        let content: MarkupReferences<any Markup>
    }

    let fields: Stored<Fields>

    /// The source range, from the opening bracket of the definition.
    public var scope: Scope { fields.scope }
    /// The optional anchor attached to this node.
    public var anchor: String? { fields.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { fields.attributes }
    /// The normalized label without the caret.
    public var id: String { fields.id }
    /// The definition's block content.
    public var content: MarkupCollection<any Markup> { fields.content }

    /// Dispatches this node to its typed visitor method.
    public func accept<V: MarkupVisitor>(_ visitor: inout V, phase: MarkupWalkPhase) -> V.Result {
        visitor.visit(self, phase: phase)
    }
}

extension Footnote.Fields {
    init(from footnote: OpaquePointer, content: [Int]) {
        var id = markdown_core_string()
        markdown_core_footnote_id(footnote, &id)
        self.init(
            scope: Scope(from: markdown_core_node_scope(footnote)),
            anchor: markdown_core_node_anchor(footnote).string,
            attributes: Attributes(from: footnote),
            id: id.required,
            content: .init(indices: content)
        )
    }
}
