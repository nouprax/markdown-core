import MarkdownCoreC

/// An inline or leaf directive — `:name[label]{key=value}` or `::name[…]{…}`.
///
/// Requires the `directives` extension. There is no placement mode: an inline
/// directive is always embedded and a ``DirectiveBlock`` always standalone, so
/// the value was implied by the kind.
public struct Directive: Markup {
    let record: DirectiveRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, its leading colon included. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The parts of its range that are its own, one per line. See ``Piece``.
    public var pieces: [Piece] { record.pieces }
    /// Where its first relation's content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The directive's name, without its colons.
    public var name: String { record.name }
    /// The bracketed label, or `nil` when the source wrote none.
    public var label: DirectiveLabel? {
        record.children.first.map { DirectiveLabel(record: unsafeDowncast($0, to: DirectiveLabelRecord.self)) }
    }
}

/// Children: the label, when the source wrote one.
final class DirectiveRecord: MarkupRecord, @unchecked Sendable {
    let name: String

    init(_ fields: InheritedFields, name: String, label: DirectiveLabelRecord?) {
        let labels: [MarkupRecord] = label.map { [$0] } ?? []
        self.name = name
        super.init(fields, children: labels)
    }

    override var markup: any Markup { Directive(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        name == unsafeDowncast(other, to: DirectiveRecord.self).name
    }
}

extension DirectiveRecord {
    convenience init(from node: OpaquePointer, label: DirectiveLabelRecord?) {
        let name = answer(markdown_core_optional_string()) {
            markdown_core_node_directive_properties(node, $0)
        }.value.required
        self.init(InheritedFields(from: node), name: name, label: label)
    }
}

extension Directive: RecordBacked {
    var base: MarkupRecord { record }
}
