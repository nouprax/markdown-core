import MarkdownCoreC

/// Ten optional metadata values, never Markup or visitor events.
public struct Metadata: Sendable, Hashable {
    /// The `name` field, absent when not authored successfully.
    public let name: MetadataValue?
    /// The `title` field, absent when not authored successfully.
    public let title: MetadataValue?
    /// The `subtitle` field, absent when not authored successfully.
    public let subtitle: MetadataValue?
    /// The `time` field, absent when not authored successfully.
    public let time: MetadataValue?
    /// The `date` field, absent when not authored successfully.
    public let date: MetadataValue?
    /// The `authors` field, absent when not authored successfully.
    public let authors: MetadataValue?
    /// The `keywords` field, absent when not authored successfully.
    public let keywords: MetadataValue?
    /// The `abstract` field, absent when not authored successfully.
    public let abstract: MetadataValue?
    /// The `state` field, absent when not authored successfully.
    public let state: MetadataValue?
    /// The `comment` field, absent when not authored successfully.
    public let comment: MetadataValue?
    /// The source extent of the complete metadata envelope.
    public let scope: Scope
    /// Creates metadata with named optional values.
    public init(
        name: MetadataValue? = nil,
        title: MetadataValue? = nil,
        subtitle: MetadataValue? = nil,
        time: MetadataValue? = nil,
        date: MetadataValue? = nil,
        authors: MetadataValue? = nil,
        keywords: MetadataValue? = nil,
        abstract: MetadataValue? = nil,
        state: MetadataValue? = nil,
        comment: MetadataValue? = nil,
        scope: Scope
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
        self.scope = scope
    }
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

extension Metadata {
    init(from metadata: OpaquePointer) {
        self.init(
            name: markdown_core_metadata_name(metadata).map { MetadataValue(from: $0) },
            title: markdown_core_metadata_title(metadata).map { MetadataValue(from: $0) },
            subtitle: markdown_core_metadata_subtitle(metadata).map { MetadataValue(from: $0) },
            time: markdown_core_metadata_time(metadata).map { MetadataValue(from: $0) },
            date: markdown_core_metadata_date(metadata).map { MetadataValue(from: $0) },
            authors: markdown_core_metadata_authors(metadata).map { MetadataValue(from: $0) },
            keywords: markdown_core_metadata_keywords(metadata).map { MetadataValue(from: $0) },
            abstract: markdown_core_metadata_abstract(metadata).map { MetadataValue(from: $0) },
            state: markdown_core_metadata_state(metadata).map { MetadataValue(from: $0) },
            comment: markdown_core_metadata_comment(metadata).map { MetadataValue(from: $0) },
            scope: Scope(from: markdown_core_metadata_scope(metadata))
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
        case MARKDOWN_CORE_METADATA_NUMBER: self = .number(scalar.value.string.requiredString)
        case MARKDOWN_CORE_METADATA_TEXT: self = .text(scalar.value.string.requiredString)
        default: preconditionFailure("Unsupported metadata scalar")
        }
    }
}

extension MetadataListItem {
    init(from item: markdown_core_metadata_list_item) {
        switch item.kind {
        case MARKDOWN_CORE_METADATA_ITEM_NUMBER: self = .number(item.value.requiredString)
        case MARKDOWN_CORE_METADATA_ITEM_TEXT: self = .text(item.value.requiredString)
        default: preconditionFailure("Unsupported metadata list item")
        }
    }
}
