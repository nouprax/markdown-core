import MarkdownCoreC

/// A footnote the document owns: a scoped value outside the markup union,
/// reached through ``Document/footnotes`` and never an element of any content
/// list.
///
/// Repeated calls share one footnote, the first definition of an id wins, and
/// a valid definition nobody calls is still a footnote. The walk reports it
/// through the ``MarkupWalkingVisitor`` case that takes a `Footnote`, after
/// the document's content.
public struct Footnote: Sendable {
    struct Fields: Sendable {
        let scope: Scope
        let id: String
        let content: [Int]
    }

    @Stored var fields: Fields

    /// The source range, from the opening bracket of the definition.
    public var scope: Scope { fields.scope }
    /// The normalized label without the caret.
    public var id: String { fields.id }
    /// The definition's block content.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }
}

extension Footnote.Fields {
    init(from footnote: OpaquePointer, content: [Int]) {
        var id = markdown_core_string()
        markdown_core_footnote_id(footnote, &id)
        self.init(
            scope: Scope(from: markdown_core_footnote_scope(footnote)),
            id: id.requiredString,
            content: content
        )
    }
}
