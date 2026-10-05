import MarkdownCoreC

/// A callout — every `>` container.
///
/// A plain quoted block is a callout without metadata: `variant`, `collapsed`
/// and `title` are `nil`. A valid opening `[!type]` line
/// populates metadata; the type is stored as written.
public struct Callout: Markup {
    let record: CalloutRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The parts of its range that are its own, one per line. See ``Piece``.
    public var pieces: [Piece] { record.pieces }
    /// Where its first relation's content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The authored type as written, or `nil` when the container has no
    /// metadata line.
    public var variant: String? { record.variant }
    /// The fold marker: `nil` when no `+` or `-` was authored, `false` for
    /// `+`, which opens expanded, and `true` for `-`.
    public var collapsed: Bool? { record.collapsed }
    /// The title's inline content, or `nil` when no title was authored; never
    /// empty. The callout owns it as a field; it is never part of `content`.
    public var title: MarkupCollection<any Markup>? {
        record.titleCount == 0 ? nil : record.collection(0..<record.titleCount)
    }
    /// The quoted blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> {
        record.collection(record.titleCount..<record.children.count)
    }
}

/// Children: the title's nodes, then the content. A present title holds at
/// least one node, so an empty title relation is an absent title.
final class CalloutRecord: MarkupRecord, @unchecked Sendable {
    let variant: String?
    let collapsed: Bool?
    let titleCount: Int

    init(
        _ fields: InheritedFields,
        variant: String?,
        collapsed: Bool?,
        title: [MarkupRecord],
        content: [MarkupRecord]
    ) {
        self.variant = variant
        self.collapsed = collapsed
        titleCount = title.count
        super.init(fields, children: title + content)
    }

    override var markup: any Markup { Callout(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: CalloutRecord.self)
        return variant == other.variant && collapsed == other.collapsed && titleCount == other.titleCount
    }

    override func relation(at step: Int) -> Relation? {
        switch step {
        case 0: Relation(name: titleCount == 0 ? nil : "Title", indices: 0..<titleCount)
        case 1: Relation(name: nil, indices: titleCount..<children.count)
        default: nil
        }
    }
}

extension CalloutRecord {
    convenience init(from node: OpaquePointer, title: [MarkupRecord], content: [MarkupRecord]) {
        var variant = markdown_core_optional_string()
        var collapsed = markdown_core_optional_bool()
        answered(markdown_core_node_callout_properties(node, &variant, &collapsed))
        self.init(
            InheritedFields(from: node),
            variant: variant.string,
            collapsed: collapsed.has_value ? collapsed.value : nil,
            title: title,
            content: content
        )
    }
}

extension Callout: RecordBacked {
    var base: MarkupRecord { record }
}
