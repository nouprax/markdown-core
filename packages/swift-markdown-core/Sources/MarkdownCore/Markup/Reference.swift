import MarkdownCoreC

/// A link reference definition — `[label]: destination "title"` — in the
/// content where it was written. It is a leaf block.
///
/// Its anchor and attributes are the ones the definition states; the links
/// and images that name it keep their own. Duplicates and unused definitions
/// remain. ``Document/references`` lists every definition in source order,
/// and ``Document/reference(for:)`` finds the node a label resolves to.
public struct Reference: Markup {
    let record: ReferenceRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The source range of the definition.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The normalized label, exactly the label of every
    /// ``Destination/reference(label:)`` that names it.
    public var label: String { record.label }
    /// Required: the ``Destination/url(_:)`` branch the definition states.
    public var dest: Destination { record.dest }
    /// Optional: `[l]: /u` wrote no title and `[l]: /u ""` wrote an empty one.
    public var title: String? { record.title }
}

final class ReferenceRecord: MarkupRecord, @unchecked Sendable {
    let label: String
    let dest: Destination
    let title: String?

    init(_ fields: InheritedFields, label: String, dest: Destination, title: String?) {
        self.label = label
        self.dest = dest
        self.title = title
        super.init(fields, children: [])
    }

    override var markup: any Markup { Reference(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: ReferenceRecord.self)
        return label == other.label && dest == other.dest && title == other.title
    }
}

extension ReferenceRecord {
    convenience init(from reference: OpaquePointer) {
        self.init(
            InheritedFields(from: reference),
            label: answer(markdown_core_string()) { markdown_core_reference_label(reference, $0) }.required,
            dest: Destination(from: reference),
            title: answer(markdown_core_optional_string()) { markdown_core_node_title(reference, $0) }.string
        )
    }
}

extension Reference: RecordBacked {
    var base: MarkupRecord { record }
}
