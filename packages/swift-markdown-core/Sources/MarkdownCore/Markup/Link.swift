import MarkdownCoreC

/// A link — `[text](destination)`, any of the three reference forms, or an
/// autolink.
///
/// A reference occurrence is the link its definition names: it answers the
/// definition's destination and title and keeps its own extent.
public struct Link: Markup {
    let record: LinkRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, brackets and parentheses included. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The link text, as inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `.url("")`; a reference occurrence answers the
    /// destination its definition stated.
    public var dest: Destination { record.dest }
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    public var title: String? { record.title }
}

final class LinkRecord: MarkupRecord, @unchecked Sendable {
    let dest: Destination
    let title: String?

    init(_ fields: InheritedFields, dest: Destination, title: String?, content: [MarkupRecord]) {
        self.dest = dest
        self.title = title
        super.init(fields, children: content)
    }

    override var markup: any Markup { Link(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: LinkRecord.self)
        return dest == other.dest && title == other.title
    }
}

extension LinkRecord {
    convenience init(
        from node: OpaquePointer,
        content: [MarkupRecord],
        resources: inout [UnsafeRawPointer: SharedResource]
    ) {
        let resource = SharedResource.shared(by: node, in: &resources)
        self.init(
            resource.fields(of: node),
            dest: resource.dest,
            title: resource.title,
            content: content
        )
    }
}

extension Link: RecordBacked {
    var base: MarkupRecord { record }
}
