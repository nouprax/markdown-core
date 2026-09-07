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
    /// A labeled example-list marker.
    case example
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
    public let scope: Scope
    /// A list owns `ListItem`s and nothing else.
    public let items: [ListItem]
    /// Bulleted or numbered.
    public let flavor: ListFlavor
    /// The first number an ordered list counts from, and `nil` for a bulleted
    /// one — which is the only reason it is optional.
    public let start: Int64?
    /// The authored numbering variant, or `nil` for a bullet list.
    public let variant: OrderedListVariant?
    /// The authored delimiter, or `nil` for a bullet list.
    public let delimiter: OrderedListDelimiter?
    /// Whether the source separated the items by blank lines. A loose list
    /// wraps each item's text in a ``Paragraph``; a tight one does not, so
    /// this is already visible in the tree and is stated here as well.
    public let tight: Bool

    /// Dispatches to the visitor's `List` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension List {
    init(from node: OpaquePointer, children: [any Markup]) {
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var variant = markdown_core_ordered_list_variant()
        var delimiter = markdown_core_ordered_list_delimiter()
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight)
        self.init(
            scope: Self.scope(from: node),
            items: Self.typedChildren(children),
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
        case MARKDOWN_CORE_ORDERED_LIST_VARIANT_EXAMPLE: .example
        case MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT: .default
        default: .decimal
        }
    }
}

/// One item of a ``List``.
public struct ListItem: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The item's blocks. Block content, not inline.
    public let content: [any Markup]
    /// The authored task marker, or `nil` when this is not a task item.
    public let marker: String?
    /// The authored example-list label; reserved until example lists land.
    public let exampleLabel: String?
    /// Whether this item authored a task marker.
    public var tasked: Bool { marker != nil }
    /// Whether this item authored a completed or custom-state task marker.
    /// Non-task items and the incomplete marker (`" "`) are not complete.
    public var completed: Bool { marker != nil && marker != " " }

    /// Dispatches to the visitor's `ListItem` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ListItem {
    init(from node: OpaquePointer, content: [any Markup]) {
        var marker = markdown_core_optional_string()
        var exampleLabel = markdown_core_optional_string()
        markdown_core_node_list_item_properties(node, &marker, &exampleLabel)
        self.init(
            scope: Self.scope(from: node),
            content: content,
            marker: marker.string,
            exampleLabel: exampleLabel.string
        )
    }
}
