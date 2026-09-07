import MarkdownCoreC

/// How a ``CitationReferent/bib(key:mode:)`` item is rendered.
public enum BibMode: String, Sendable, Hashable {
    /// The default rendering, author and date in parentheses.
    case normal
    /// The author in the running text, only the date in parentheses.
    case authorInText
    /// The date alone, the author suppressed.
    case suppressAuthor
}

/// What a ``Citation`` names: a tagged value, so a branch's fields exist only
/// in that branch.
public enum CitationReferent: Sendable, Hashable {
    /// A bibliography key with its mode. No parse produces this branch until
    /// `P7`.
    case bib(key: String, mode: BibMode)
    /// A footnote named by id: the normalized label without the caret, as
    /// ``Footnote/id`` states it.
    case footnote(id: String)
}

/// One item of a ``Cite``: a scoped value the cite owns, outside the markup
/// union.
///
/// It has no ``MarkupVisitor`` case; the walk reports it through the
/// ``MarkupWalkingVisitor`` case that takes a `Citation`, between the cite's
/// entering and exiting.
public struct Citation: Sendable {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// What it names.
    public let referent: CitationReferent
    /// The inline content before the referent, owned by the citation; empty
    /// for an inherited call.
    public let prefix: [any Markup]
    /// The inline content after the referent, owned by the citation; empty
    /// for an inherited call.
    public let suffix: [any Markup]
}

/// An inline citation: one or more ``Citation`` items in authored order.
///
/// An inherited `[^label]` call is a one-item cite naming its footnote; the
/// citation syntaxes module, which fills in ``CitationReferent/bib(key:mode:)``
/// items, lands with `P7`. Its items are scoped values, not content, so a
/// cite is a leaf.
public struct Cite: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// Never empty: every cite is authored with at least one item.
    public let citations: [Citation]

    /// Dispatches to the visitor's `Cite` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension BibMode {
    init(from mode: markdown_core_bib_mode) {
        switch mode {
        case MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT: self = .authorInText
        case MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR: self = .suppressAuthor
        default: self = .normal
        }
    }
}

extension CitationReferent {
    /// A branch's fields exist only in that branch, so only they are copied.
    init(from citation: OpaquePointer) {
        var referent = markdown_core_referent()
        markdown_core_citation_referent(citation, &referent)
        switch referent.kind {
        case MARKDOWN_CORE_REFERENT_BIB:
            self = .bib(key: referent.key.requiredString, mode: BibMode(from: referent.mode))
        default:
            self = .footnote(id: referent.id.requiredString)
        }
    }
}

extension Citation {
    init(from citation: OpaquePointer, prefix: [any Markup], suffix: [any Markup]) {
        self.init(
            scope: Scope(from: markdown_core_citation_scope(citation)),
            referent: CitationReferent(from: citation),
            prefix: prefix,
            suffix: suffix
        )
    }
}
