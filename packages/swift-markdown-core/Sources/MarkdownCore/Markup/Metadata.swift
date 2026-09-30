import MarkdownCoreC

/// A leaf Markup node containing ten optional metadata values.
public struct Metadata: Markup {
    let record: MetadataRecord

    /// The `name` field, absent when not authored successfully.
    public var name: MetadataValue? { record.name }
    /// The `title` field, absent when not authored successfully.
    public var title: MetadataValue? { record.title }
    /// The `subtitle` field, absent when not authored successfully.
    public var subtitle: MetadataValue? { record.subtitle }
    /// The `time` field, absent when not authored successfully.
    public var time: MetadataValue? { record.time }
    /// The `date` field, absent when not authored successfully.
    public var date: MetadataValue? { record.date }
    /// The `authors` field, absent when not authored successfully.
    public var authors: MetadataValue? { record.authors }
    /// The `keywords` field, absent when not authored successfully.
    public var keywords: MetadataValue? { record.keywords }
    /// The `abstract` field, absent when not authored successfully.
    public var abstract: MetadataValue? { record.abstract }
    /// The `state` field, absent when not authored successfully.
    public var state: MetadataValue? { record.state }
    /// The `comment` field, absent when not authored successfully.
    public var comment: MetadataValue? { record.comment }
    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The source extent of the complete metadata envelope.
    public var extent: Extent { record.extent }
    /// The optional anchor attached to this node.
    public var anchor: String? { record.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { record.attributes }
}

final class MetadataRecord: MarkupRecord, @unchecked Sendable {
    let name: MetadataValue?
    let title: MetadataValue?
    let subtitle: MetadataValue?
    let time: MetadataValue?
    let date: MetadataValue?
    let authors: MetadataValue?
    let keywords: MetadataValue?
    let abstract: MetadataValue?
    let state: MetadataValue?
    let comment: MetadataValue?

    init(
        _ fields: InheritedFields,
        name: MetadataValue? = nil,
        title: MetadataValue? = nil,
        subtitle: MetadataValue? = nil,
        time: MetadataValue? = nil,
        date: MetadataValue? = nil,
        authors: MetadataValue? = nil,
        keywords: MetadataValue? = nil,
        abstract: MetadataValue? = nil,
        state: MetadataValue? = nil,
        comment: MetadataValue? = nil
    ) {
        self.name = name
        self.title = title
        self.subtitle = subtitle
        self.time = time
        self.date = date
        self.authors = authors
        self.keywords = keywords
        self.abstract = abstract
        self.state = state
        self.comment = comment
        super.init(fields, children: [])
    }

    override var markup: any Markup { Metadata(record: self) }

    /// The ten fields in contract order.
    var values: [MetadataValue?] { [name, title, subtitle, time, date, authors, keywords, abstract, state, comment] }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        values == unsafeDowncast(other, to: MetadataRecord.self).values
    }
}

extension Metadata: RecordBacked {
    var base: MarkupRecord { record }
}

/// A scalar or an ordered list; an empty list is distinct from null.
public enum MetadataValue: Sendable, Hashable {
    /// One typed scalar.
    case scalar(MetadataScalar)
    /// An ordered sequence of list members.
    case list([MetadataListItem])
}
/// A Properties scalar, retaining decimal numbers as exact text.
public enum MetadataScalar: Sendable, Hashable {
    /// An explicitly null scalar.
    case null
    /// A boolean scalar.
    case bool(Bool)
    /// Exact decimal text, without floating-point conversion.
    case number(String)
    /// Text, including an explicitly empty string.
    case text(String)
}
/// A Properties list member: exact decimal text or text.
public enum MetadataListItem: Sendable, Hashable {
    /// Exact decimal text, without floating-point conversion.
    case number(String)
    /// Text, including an explicitly empty string.
    case text(String)
}

extension MetadataRecord {
    convenience init(from metadata: OpaquePointer) {
        self.init(
            InheritedFields(from: metadata),
            name: markdown_core_metadata_name(metadata).map { MetadataValue(from: $0) },
            title: markdown_core_metadata_title(metadata).map { MetadataValue(from: $0) },
            subtitle: markdown_core_metadata_subtitle(metadata).map { MetadataValue(from: $0) },
            time: markdown_core_metadata_time(metadata).map { MetadataValue(from: $0) },
            date: markdown_core_metadata_date(metadata).map { MetadataValue(from: $0) },
            authors: markdown_core_metadata_authors(metadata).map { MetadataValue(from: $0) },
            keywords: markdown_core_metadata_keywords(metadata).map { MetadataValue(from: $0) },
            abstract: markdown_core_metadata_abstract(metadata).map { MetadataValue(from: $0) },
            state: markdown_core_metadata_state(metadata).map { MetadataValue(from: $0) },
            comment: markdown_core_metadata_comment(metadata).map { MetadataValue(from: $0) }
        )
    }
}

extension MetadataValue {
    init(from record: OpaquePointer) {
        switch markdown_core_metadata_value_get_kind(record) {
        case MARKDOWN_CORE_METADATA_SCALAR:
            var scalar = markdown_core_metadata_scalar()
            precondition(markdown_core_metadata_value_scalar(record, &scalar))
            self = .scalar(MetadataScalar(from: scalar))
        case MARKDOWN_CORE_METADATA_LIST:
            self = .list(
                (0..<markdown_core_metadata_value_item_count(record)).map { index in
                    var item = markdown_core_metadata_list_item()
                    precondition(markdown_core_metadata_value_item_at(record, index, &item))
                    return MetadataListItem(from: item)
                }
            )
        default: preconditionFailure("Unsupported metadata value")
        }
    }
}

extension MetadataScalar {
    init(from scalar: markdown_core_metadata_scalar) {
        switch scalar.kind {
        case MARKDOWN_CORE_METADATA_NULL: self = .null
        case MARKDOWN_CORE_METADATA_BOOL: self = .bool(scalar.value.boolean)
        case MARKDOWN_CORE_METADATA_NUMBER: self = .number(scalar.value.string.required)
        case MARKDOWN_CORE_METADATA_TEXT: self = .text(scalar.value.string.required)
        default: preconditionFailure("Unsupported metadata scalar")
        }
    }
}

extension MetadataListItem {
    init(from item: markdown_core_metadata_list_item) {
        switch item.kind {
        case MARKDOWN_CORE_METADATA_ITEM_NUMBER: self = .number(item.value.required)
        case MARKDOWN_CORE_METADATA_ITEM_TEXT: self = .text(item.value.required)
        default: preconditionFailure("Unsupported metadata list item")
        }
    }
}
