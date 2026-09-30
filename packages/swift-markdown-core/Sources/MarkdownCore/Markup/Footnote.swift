import MarkdownCoreC

/// A footnote: a definition `[^label]: body` in the content where it was
/// written, or an inline note `^[body]` owned by its citation's referent.
///
/// A definition's label is the normalized label without the caret; an inline
/// note has none. ``Document/footnotes`` lists every footnote in source order,
/// and ``Document/footnote(for:)`` finds the first definition of a label.
public struct Footnote: Markup {
    let record: FootnoteRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The source range, from the opening bracket of the definition or the
    /// caret of an inline note.
    public var extent: Extent { record.extent }
    /// The optional anchor attached to this node.
    public var anchor: String? { record.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { record.attributes }
    /// The normalized label without the caret, or `nil` for an inline note.
    public var label: String? { record.label }
    /// The definition's block content, or an inline note's inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class FootnoteRecord: MarkupRecord, @unchecked Sendable {
    let label: String?

    init(_ fields: InheritedFields, label: String?, content: [MarkupRecord]) {
        self.label = label
        super.init(fields, children: content)
    }

    override var markup: any Markup { Footnote(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        label == unsafeDowncast(other, to: FootnoteRecord.self).label
    }
}

extension FootnoteRecord {
    convenience init(from footnote: OpaquePointer, content: [MarkupRecord]) {
        var label = markdown_core_optional_string()
        precondition(markdown_core_footnote_label(footnote, &label), "Invalid native footnote")
        self.init(InheritedFields(from: footnote), label: label.string, content: content)
    }
}

extension Footnote: RecordBacked {
    var base: MarkupRecord { record }
}
