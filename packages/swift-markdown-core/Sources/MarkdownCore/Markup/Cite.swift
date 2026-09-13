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
    /// A labeled specimen definition owned by the document; first produced by P9b.
    case specimen(id: String)
}

/// A Markup node owned by a cite's citations field.
/// Its prefix and suffix contain inline markup and are empty when absent.
public struct Citation: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let referent: CitationReferent
        let prefix: MarkupReferences<any Markup>
        let suffix: MarkupReferences<any Markup>
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The optional anchor attached to this node.
    public var anchor: String? { fields.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { fields.attributes }
    /// What it names.
    public var referent: CitationReferent { fields.referent }
    /// The inline content before the referent, owned by the citation; empty
    /// for an inherited call.
    public var prefix: MarkupCollection<any Markup> { fields.prefix }
    /// The inline content after the referent, owned by the citation; empty
    /// for an inherited call.
    public var suffix: MarkupCollection<any Markup> { fields.suffix }

    /// Dispatches this node to its typed visitor method.
    public func accept<V: MarkupVisitor>(_ visitor: inout V, phase: MarkupVisitPhase) -> V.Result {
        visitor.visit(self, phase: phase)
    }
}

/// An inline citation cluster owning one or more Citation nodes in source order.
/// A footnote call contains one item with an ID referent and empty affixes.
public struct Cite: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let citations: MarkupReferences<Citation>
    }

    let fields: Stored<Fields>

    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// Never empty: every cite is authored with at least one item.
    public var citations: MarkupCollection<Citation> { fields.citations }

    /// Dispatches to the visitor's `Cite` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V, phase: MarkupVisitPhase) -> V.Result {
        visitor.visit(self, phase: phase)
    }
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
            self = .bib(key: referent.key.required, mode: BibMode(from: referent.mode))
        case MARKDOWN_CORE_REFERENT_FOOTNOTE:
            self = .footnote(id: referent.id.required)
        case MARKDOWN_CORE_REFERENT_SPECIMEN:
            self = .specimen(id: referent.id.required)
        default:
            preconditionFailure("Unsupported native citation referent")
        }
    }
}

extension Citation.Fields {
    init(from citation: OpaquePointer, prefix: [Int], suffix: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(citation)),
            anchor: markdown_core_node_anchor(citation).string,
            attributes: Attributes(from: citation),
            referent: CitationReferent(from: citation),
            prefix: .init(indices: prefix),
            suffix: .init(indices: suffix)
        )
    }
}

extension Cite.Fields {
    init(from node: OpaquePointer, citations: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            citations: .init(indices: citations)
        )
    }
}
