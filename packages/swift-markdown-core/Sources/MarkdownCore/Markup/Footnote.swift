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
    /// The source range, from the opening bracket of the definition.
    public let scope: Scope
    /// The normalized label without the caret.
    public let id: String
    /// The definition's block content.
    public let content: [any Markup]
}

extension Footnote {
    init(from footnote: OpaquePointer, content: [any Markup]) {
        var id = markdown_core_string()
        markdown_core_footnote_id(footnote, &id)
        self.init(
            scope: Scope(from: markdown_core_footnote_scope(footnote)),
            id: id.requiredString,
            content: content
        )
    }
}
