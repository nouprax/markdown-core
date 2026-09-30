import MarkdownCoreC

/// A specimen definition in the content where it was written.
///
/// ``Document/specimens`` lists every definition in source order, and
/// ``Document/specimen(for:)`` finds the first definition of a label. Display
/// numbering is derived by consumers.
public struct Specimen: Markup {
    let record: SpecimenRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The source range of the definition.
    public var extent: Extent { record.extent }
    /// The optional anchor attached to this node.
    public var anchor: String? { record.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { record.attributes }
    /// The authored label, or `nil` for an anonymous definition.
    public var label: String? { record.label }
    /// An explicit counter reset, or `nil` when numbering continues.
    public var start: Int64? { record.start }
    /// The parsed content of the definition.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
}

final class SpecimenRecord: MarkupRecord, @unchecked Sendable {
    let label: String?
    let start: Int64?

    init(_ fields: InheritedFields, label: String?, start: Int64?, content: [MarkupRecord]) {
        self.label = label
        self.start = start
        super.init(fields, children: content)
    }

    override var markup: any Markup { Specimen(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: SpecimenRecord.self)
        return label == other.label && start == other.start
    }
}

extension SpecimenRecord {
    convenience init(from specimen: OpaquePointer, content: [MarkupRecord]) {
        var label = markdown_core_optional_string()
        var start = markdown_core_optional_i64()
        precondition(markdown_core_specimen_properties(specimen, &label, &start), "Invalid native specimen")
        self.init(
            InheritedFields(from: specimen),
            label: label.string,
            start: start.has_value ? start.value : nil,
            content: content
        )
    }
}

extension Specimen: RecordBacked {
    var base: MarkupRecord { record }
}
