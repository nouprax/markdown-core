import MarkdownCoreC

/// An inline embed — `![alt](source)` or a resolved reference.
/// The target type is not inferred.
///
/// Its content is PARSED alt text: `![a *b*](s)` has an ``Emphasis`` in it, and
/// flattening it to a string is the consumer's decision, not the parser's.
/// Complete `W`, `WxH`, `alt|W` and `alt|WxH` labels supply positive 32-bit
/// dimensions without leading zeros, on both direct and resolved images.
public struct Embedded: Markup {
    let record: EmbeddedRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, `![` through the closing parenthesis. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// Parsed alt content excluding a valid dimension suffix; empty for a numeric-only label.
    /// A malformed suffix remains part of the alt content.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Required, for the reason ``Link/dest`` is.
    public var dest: Destination { record.dest }
    /// Optional.
    public var title: String? { record.title }
    /// Authored size from a complete label suffix, or nil. Independent of attribute records.
    public var dimensions: Dimensions? { record.dimensions }
}

final class EmbeddedRecord: MarkupRecord, @unchecked Sendable {
    let dest: Destination
    let title: String?
    let dimensions: Dimensions?

    init(
        _ fields: InheritedFields,
        dest: Destination,
        title: String?,
        dimensions: Dimensions?,
        content: [MarkupRecord]
    ) {
        self.dest = dest
        self.title = title
        self.dimensions = dimensions
        super.init(fields, children: content)
    }

    override var markup: any Markup { Embedded(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: EmbeddedRecord.self)
        return dest == other.dest && title == other.title && dimensions == other.dimensions
    }
}

extension EmbeddedRecord {
    convenience init(
        from node: OpaquePointer,
        content: [MarkupRecord],
        resources: inout [Int: SharedResource]
    ) {
        let resource = SharedResource.shared(by: node, in: &resources)
        self.init(
            resource.fields(of: node),
            dest: resource.dest,
            title: resource.title,
            dimensions: markdown_core_node_dimensions(node).map { Dimensions($0.pointee) },
            content: content
        )
    }
}

extension Embedded: RecordBacked {
    var base: MarkupRecord { record }
}
