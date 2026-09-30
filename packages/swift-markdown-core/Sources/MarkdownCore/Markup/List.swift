import MarkdownCoreC

/// Whether a list is bulleted or numbered.
///
/// It does not record WHICH marker was used: `-`, `*` and `+` are all
/// ``bullet``, and `1.` and `1)` are both ``ordered``.
public enum ListFlavor: String, Sendable {
    /// `-`, `*` or `+`.
    case bullet
    /// A number followed by `.` or `)`.
    case ordered
}

/// The numbering system authored for an ordered list.
public enum OrderedListVariant: Hashable, Sendable {
    /// ASCII decimal digits.
    case decimal
    /// ASCII letters, recording their authored case.
    case alpha(lowercased: Bool)
    /// Roman numerals, recording their authored case.
    case roman(lowercased: Bool)
    /// A source marker that requests the default numbering variant.
    case `default`
}

/// The punctuation authored around an ordered-list marker.
public enum OrderedListDelimiter: Hashable, Sendable {
    /// A trailing period, as in `1.`.
    case period
    /// Parenthesis punctuation. `closed` is false for `1)` and true for `(1)`.
    case parenthesis(closed: Bool)
    /// A source marker that requests the default delimiter.
    case `default`
}

/// A bulleted or numbered list.
public struct List: Markup {
    let record: ListRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// A list owns `ListItem`s and nothing else.
    public var items: MarkupCollection<ListItem> { record.collection(record.children.indices) }
    /// Bulleted or numbered.
    public var flavor: ListFlavor { record.flavor }
    /// The first number an ordered list counts from, and `nil` for a bulleted
    /// one — which is the only reason it is optional.
    public var start: Int64? { record.start }
    /// The authored numbering variant, or `nil` for a bullet list.
    public var variant: OrderedListVariant? { record.variant }
    /// The authored delimiter, or `nil` for a bullet list.
    public var delimiter: OrderedListDelimiter? { record.delimiter }
    /// Whether the source separated the items by blank lines. A loose list
    /// wraps each item's text in a ``Paragraph``; a tight one does not, so
    /// this is already visible in the tree and is stated here as well.
    public var tight: Bool { record.tight }
}

final class ListRecord: MarkupRecord, @unchecked Sendable {
    let flavor: ListFlavor
    let start: Int64?
    let variant: OrderedListVariant?
    let delimiter: OrderedListDelimiter?
    let tight: Bool

    init(
        _ fields: InheritedFields,
        flavor: ListFlavor,
        start: Int64?,
        variant: OrderedListVariant?,
        delimiter: OrderedListDelimiter?,
        tight: Bool,
        items: [MarkupRecord]
    ) {
        self.flavor = flavor
        self.start = start
        self.variant = variant
        self.delimiter = delimiter
        self.tight = tight
        super.init(fields, children: items)
    }

    override var markup: any Markup { List(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: ListRecord.self)
        return flavor == other.flavor && start == other.start && variant == other.variant
            && delimiter == other.delimiter && tight == other.tight
    }
}

extension ListRecord {
    convenience init(from node: OpaquePointer, items: [MarkupRecord]) {
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var variant = markdown_core_ordered_list_variant()
        var delimiter = markdown_core_ordered_list_delimiter()
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight)
        self.init(
            InheritedFields(from: node),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            variant: start.has_value ? Self.variant(variant) : nil,
            delimiter: start.has_value ? Self.delimiter(delimiter) : nil,
            tight: tight,
            items: items
        )
    }

    static func delimiter(_ value: markdown_core_ordered_list_delimiter) -> OrderedListDelimiter {
        switch value.kind {
        case MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD: .period
        case MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS: .parenthesis(closed: value.closed)
        case MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT: .default
        default: preconditionFailure("Unsupported native list delimiter \(value.kind)")
        }
    }

    private static func variant(_ value: markdown_core_ordered_list_variant) -> OrderedListVariant {
        switch value.kind {
        case MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA: .alpha(lowercased: value.lowercased)
        case MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN: .roman(lowercased: value.lowercased)
        case MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT: .default
        default: .decimal
        }
    }
}

extension List: RecordBacked {
    var base: MarkupRecord { record }
}

/// One item of a ``List``.
public struct ListItem: Markup {
    let record: ListItemRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The item's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// The authored task marker, or `nil` when this is not a task item.
    public var marker: String? { record.marker }
    /// Whether this item authored a task marker.
    public var tasked: Bool { marker != nil }
    /// Whether this item authored a completed or custom-state task marker.
    /// Non-task items and the incomplete marker (`" "`) are not complete.
    public var completed: Bool { marker != nil && marker != " " }
}

final class ListItemRecord: MarkupRecord, @unchecked Sendable {
    let marker: String?

    init(_ fields: InheritedFields, marker: String?, content: [MarkupRecord]) {
        self.marker = marker
        super.init(fields, children: content)
    }

    override var markup: any Markup { ListItem(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        marker == unsafeDowncast(other, to: ListItemRecord.self).marker
    }
}

extension ListItemRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        var marker = markdown_core_optional_string()
        markdown_core_node_list_item_marker(node, &marker)
        self.init(InheritedFields(from: node), marker: marker.string, content: content)
    }
}

extension ListItem: RecordBacked {
    var base: MarkupRecord { record }
}
