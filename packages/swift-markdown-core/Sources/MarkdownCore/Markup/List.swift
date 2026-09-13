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
public enum OrderedListVariant: Equatable, Sendable {
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
public enum OrderedListDelimiter: Equatable, Sendable {
    /// A trailing period, as in `1.`.
    case period
    /// Parenthesis punctuation. `closed` is false for `1)` and true for `(1)`.
    case parenthesis(closed: Bool)
    /// A source marker that requests the default delimiter.
    case `default`
}

/// A bulleted or numbered list.
public struct List: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// A list owns `ListItem`s and nothing else.
    public var items: MarkupCollection<ListItem> { $fields.collection(fields.items) }
    /// Bulleted or numbered.
    public var flavor: ListFlavor { fields.flavor }
    /// The first number an ordered list counts from, and `nil` for a bulleted
    /// one — which is the only reason it is optional.
    public var start: Int64? { fields.start }
    /// The authored numbering variant, or `nil` for a bullet list.
    public var variant: OrderedListVariant? { fields.variant }
    /// The authored delimiter, or `nil` for a bullet list.
    public var delimiter: OrderedListDelimiter? { fields.delimiter }
    /// Whether the source separated the items by blank lines. A loose list
    /// wraps each item's text in a ``Paragraph``; a tight one does not, so
    /// this is already visible in the tree and is stated here as well.
    public var tight: Bool { fields.tight }

    /// Dispatches to the visitor's `List` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let items: [Int]
        let flavor: ListFlavor
        let start: Int64?
        let variant: OrderedListVariant?
        let delimiter: OrderedListDelimiter?
        let tight: Bool
    }

    @Stored var fields: Fields
}

extension List.Fields {
    init(from node: OpaquePointer, children: [Int]) {
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var variant = markdown_core_ordered_list_variant()
        var delimiter = markdown_core_ordered_list_delimiter()
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            items: children,
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            variant: start.has_value ? Self.variant(variant) : nil,
            delimiter: start.has_value ? Self.delimiter(delimiter) : nil,
            tight: tight
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

/// One item of a ``List``.
public struct ListItem: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The item's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }
    /// The authored task marker, or `nil` when this is not a task item.
    public var marker: String? { fields.marker }
    /// Whether this item authored a task marker.
    public var tasked: Bool { marker != nil }
    /// Whether this item authored a completed or custom-state task marker.
    /// Non-task items and the incomplete marker (`" "`) are not complete.
    public var completed: Bool { marker != nil && marker != " " }

    /// Dispatches to the visitor's `ListItem` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
        let marker: String?
    }

    @Stored var fields: Fields
}

extension ListItem.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        var marker = markdown_core_optional_string()
        markdown_core_node_list_item_marker(node, &marker)
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            marker: marker.string,
        )
    }
}
