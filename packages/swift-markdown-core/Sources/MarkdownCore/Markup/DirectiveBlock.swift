import MarkdownCoreC

/// A named leaf or container directive, or a nameless fenced container.
///
/// Nameless containers use `::: {.class}` or `::: class` and have a nil name.
/// All container forms share the same closing-fence and block-content rules.
public struct DirectiveBlock: Markup {
    let record: DirectiveBlockRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, opening fence through closing fence. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The directive's name without colons, or nil for a nameless container.
    public var name: String? { record.name }
    /// The bracketed label, or `nil` when the source wrote none.
    public var label: DirectiveLabel? {
        record.labelCount == 0
            ? nil : DirectiveLabel(record: unsafeDowncast(record.children[0], to: DirectiveLabelRecord.self))
    }
    /// The block content the fence encloses.
    public var content: MarkupCollection<any Markup> {
        record.collection(record.labelCount..<record.children.count)
    }
}

/// Children: the label, when the source wrote one, then the content.
final class DirectiveBlockRecord: MarkupRecord, @unchecked Sendable {
    let name: String?
    let labelCount: Int

    init(_ fields: InheritedFields, name: String?, label: DirectiveLabelRecord?, content: [MarkupRecord]) {
        self.name = name
        let labels: [MarkupRecord] = label.map { [$0] } ?? []
        labelCount = labels.count
        super.init(fields, children: labels + content)
    }

    override var markup: any Markup { DirectiveBlock(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: DirectiveBlockRecord.self)
        return name == other.name && labelCount == other.labelCount
    }

    override func relation(at step: Int) -> Relation? {
        switch step {
        case 0: Relation(name: nil, indices: 0..<labelCount)
        case 1: Relation(name: nil, indices: labelCount..<children.count)
        default: nil
        }
    }
}

extension DirectiveBlockRecord {
    convenience init(from node: OpaquePointer, label: DirectiveLabelRecord?, content: [MarkupRecord]) {
        let name = markdown_core_node_directive_properties(node).string
        self.init(InheritedFields(from: node), name: name, label: label, content: content)
    }
}

extension DirectiveBlock: RecordBacked {
    var base: MarkupRecord { record }
}
