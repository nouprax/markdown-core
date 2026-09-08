import MarkdownCoreC

/// Properties are values, never Markup or visitor events.
public struct Metadata: Sendable, Hashable {
    /// Data and uninterpreted source fragments in authored order.
    public let content: [MetadataContent]
    /// The source extent of this value.
    public let scope: Scope
    /// Creates metadata with its ordered content and source extent.
    public init(content: [MetadataContent], scope: Scope) {
        self.content = content
        self.scope = scope
    }
}
/// An interpreted property or source retained without interpretation; never Markup.
public enum MetadataContent: Sendable, Hashable {
    /// Authored source that is a comment or cannot be represented as property data.
    case comment(String)
    /// One named property.
    case data(MetadataRecord)
}
/// One named Properties value, with its own source extent.
public struct MetadataRecord: Sendable, Hashable {
    /// The exact case-sensitive property name.
    public let name: String
    /// The tagged property value.
    public let value: MetadataValue
    /// The source extent of this value.
    public let scope: Scope
    /// Creates a property without interpreting its name.
    public init(name: String, value: MetadataValue, scope: Scope) {
        self.name = name
        self.value = value
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
            content: (0..<markdown_core_metadata_content_count(metadata)).map { index in
                guard let content = markdown_core_metadata_content_at(metadata, index) else {
                    preconditionFailure("Metadata content count is inconsistent")
                }
                switch markdown_core_metadata_content_get_kind(content) {
                case MARKDOWN_CORE_METADATA_COMMENT:
                    return .comment(markdown_core_metadata_content_comment(content).requiredString)
                case MARKDOWN_CORE_METADATA_DATA:
                    guard let record = markdown_core_metadata_content_data(content) else {
                        preconditionFailure("Metadata data is missing its record")
                    }
                    return .data(
                        MetadataRecord(
                            name: markdown_core_metadata_record_name(record).requiredString,
                            value: MetadataValue(from: record),
                            scope: Scope(from: markdown_core_metadata_record_scope(record))
                        )
                    )
                default: preconditionFailure("Unsupported metadata content")
                }
            },
            scope: Scope(from: markdown_core_metadata_scope(metadata))
        )
    }
}

extension MetadataValue {
    init(from record: OpaquePointer) {
        switch markdown_core_metadata_record_kind(record) {
        case MARKDOWN_CORE_METADATA_SCALAR:
            var scalar = markdown_core_metadata_scalar()
            precondition(markdown_core_metadata_record_scalar(record, &scalar))
            self = .scalar(MetadataScalar(from: scalar))
        case MARKDOWN_CORE_METADATA_LIST:
            self = .list(
                (0..<markdown_core_metadata_record_item_count(record)).map { index in
                    var item = markdown_core_metadata_list_item()
                    precondition(markdown_core_metadata_record_item_at(record, index, &item))
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
