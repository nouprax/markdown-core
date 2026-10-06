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

/// The footnote a footnote referent names: a label looked up in the document,
/// or an inline note's ``Footnote``, which the referent owns.
public enum FootnoteTarget: Sendable, Hashable {
    /// The normalized label without the caret. ``Document/footnote(for:)``
    /// finds the first definition of it.
    case label(value: String)
    /// The inline note `^[body]`, owned at its call site.
    case note(footnote: Footnote)
}

/// What a ``Citation`` names: a tagged value, so a branch's fields exist only
/// in that branch.
public enum CitationReferent: Sendable, Hashable {
    /// A bibliography key with its mode.
    case bib(key: String, mode: BibMode)
    /// A footnote, named by label or owned as an inline note.
    case footnote(target: FootnoteTarget)
    /// A specimen named by its normalized label. ``Document/specimen(for:)``
    /// finds the first definition of it.
    case specimen(label: String)
}

/// A Markup node owned by a cite's citations field.
/// Its prefix and suffix contain inline markup and are empty when absent.
public struct Citation: Markup {
    let record: CitationRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The optional anchor attached to this node.
    public var anchor: String? { record.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { record.attributes }
    /// What it names.
    public var referent: CitationReferent {
        switch record.referent {
        case .bib(let key, let mode): .bib(key: key, mode: mode)
        case .label(let label): .footnote(target: .label(value: label))
        case .note:
            .footnote(target: .note(footnote: Footnote(record: record.note)))
        case .specimen(let label): .specimen(label: label)
        }
    }
    /// The inline content before the referent, owned by the citation; empty
    /// for an inherited call.
    public var prefix: MarkupCollection<any Markup> {
        record.collection(record.noteCount..<(record.noteCount + record.prefixCount))
    }
    /// The inline content after the referent, owned by the citation; empty
    /// for an inherited call.
    public var suffix: MarkupCollection<any Markup> {
        record.collection((record.noteCount + record.prefixCount)..<record.children.count)
    }
}

/// Children: an inline note's footnote, then the prefix, then the suffix.
final class CitationRecord: MarkupRecord, @unchecked Sendable {
    /// The referent's scalars. An inline note is the first child, not a field.
    enum Referent: Hashable, Sendable {
        case bib(key: String, mode: BibMode)
        case label(String)
        case note
        case specimen(label: String)
    }

    let referent: Referent
    let prefixCount: Int

    init(
        _ fields: InheritedFields,
        referent: Referent,
        note: FootnoteRecord?,
        prefix: [MarkupRecord],
        suffix: [MarkupRecord]
    ) {
        let notes: [MarkupRecord] = note.map { [$0] } ?? []
        self.referent = referent
        prefixCount = prefix.count
        super.init(fields, children: notes + prefix + suffix)
    }

    var noteCount: Int { referent == .note ? 1 : 0 }

    /// The inline note's footnote; only for a ``Referent/note`` referent.
    var note: FootnoteRecord { unsafeDowncast(children[0], to: FootnoteRecord.self) }

    override var markup: any Markup { Citation(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: CitationRecord.self)
        return referent == other.referent && prefixCount == other.prefixCount
    }

    override func relation(at step: Int) -> Relation? {
        let split = noteCount + prefixCount
        return switch step {
        case 0: Relation(name: nil, indices: 0..<noteCount)
        case 1: Relation(name: "CitationPrefix", indices: noteCount..<split)
        case 2: Relation(name: "CitationSuffix", indices: split..<children.count)
        default: nil
        }
    }
}

/// An inline citation cluster owning one or more Citation nodes in source order.
/// A footnote call contains one item with a footnote referent and empty affixes.
public struct Cite: Markup {
    let record: CiteRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// Never empty: every cite is authored with at least one item.
    public var citations: MarkupCollection<Citation> { record.collection(record.children.indices) }
}

final class CiteRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { Cite(record: self) }
}

extension BibMode {
    /// Indexed by the native value less one.
    private static let native: [BibMode] = [.normal, .authorInText, .suppressAuthor]

    init(from mode: markdown_core_bib_mode) {
        self = BibMode.native[Int(mode.rawValue) - 1]
    }
}

extension CitationRecord {
    /// A branch's fields exist only in that branch, so only they are copied.
    convenience init(
        from citation: OpaquePointer,
        note: FootnoteRecord?,
        prefix: [MarkupRecord],
        suffix: [MarkupRecord]
    ) {
        let native = answer(markdown_core_referent()) { markdown_core_citation_referent(citation, $0) }
        let referent: Referent
        switch native.kind {
        case MARKDOWN_CORE_REFERENT_BIB:
            referent = .bib(key: native.key.required, mode: BibMode(from: native.mode))
        case MARKDOWN_CORE_REFERENT_FOOTNOTE:
            referent = native.note == nil ? .label(native.label.required) : .note
        // A C enum switch is never exhaustive in Swift; the one kind
        // left is MARKDOWN_CORE_REFERENT_SPECIMEN.
        default:
            referent = .specimen(label: native.label.required)
        }
        self.init(InheritedFields(from: citation), referent: referent, note: note, prefix: prefix, suffix: suffix)
    }
}

extension CiteRecord {
    convenience init(from node: OpaquePointer, citations: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: citations)
    }
}

extension Citation: RecordBacked {
    var base: MarkupRecord { record }
}

extension Cite: RecordBacked {
    var base: MarkupRecord { record }
}
