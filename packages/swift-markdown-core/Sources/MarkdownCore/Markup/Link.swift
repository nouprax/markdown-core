import MarkdownCoreC

/// A link — `[text](destination)`, any of the three reference forms, or an
/// autolink.
///
/// A reference occurrence names its definition by label: it answers the
/// ``Destination/reference(label:)`` branch, no title, and its own extent,
/// anchor and attributes. The ``Reference`` it names states the rest.
public struct Link: Markup {
    let record: LinkRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, brackets and parentheses included. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The link text, as inline content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `.url("")`; a reference occurrence answers the
    /// label it names.
    public var dest: Destination { record.dest }
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    /// A reference occurrence writes none.
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
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(
            InheritedFields(from: node),
            dest: Destination(from: node),
            title: answer(markdown_core_optional_string()) { markdown_core_node_title(node, $0) }.string,
            content: content
        )
    }
}

extension Link: RecordBacked {
    var base: MarkupRecord { record }
}
