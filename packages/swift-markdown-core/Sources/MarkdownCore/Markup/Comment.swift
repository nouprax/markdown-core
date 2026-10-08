import MarkdownCoreC

/// A comment: an inline HTML comment token, or an HTML block that opened with
/// `<!--` and closed on a `-->` line. The one kind valid in both block and
/// inline content; the parent records which.
public struct Comment: Markup {
    let record: CommentRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The bytes between the delimiters, exactly as written.
    public var literal: String { record.literal }
}

final class CommentRecord: MarkupRecord, @unchecked Sendable {
    let literal: String

    init(_ fields: InheritedFields, literal: String) {
        self.literal = literal
        super.init(fields, children: [])
    }

    override var markup: any Markup { Comment(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        literal == unsafeDowncast(other, to: CommentRecord.self).literal
    }
}

extension CommentRecord {
    convenience init(from node: OpaquePointer) {
        self.init(InheritedFields(from: node), literal: nativeLiteral(of: node))
    }
}

extension Comment: RecordBacked {
    var base: MarkupRecord { record }
}
