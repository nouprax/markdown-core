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

public enum OrderedListStyle: String, Sendable {
    case decimal, lowerAlpha, upperAlpha, lowerRoman, upperRoman, example, `default`
}

public enum OrderedListDelimiter: String, Sendable {
    case period, oneParen, twoParens, `default`
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
    /// The authored numbering style, or `nil` for a bullet list.
    public let style: OrderedListStyle?
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
        var style = MARKDOWN_CORE_ORDERED_LIST_STYLE_DECIMAL
        var delimiter = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &style, &delimiter, &tight)
        self.init(
            scope: Self.scope(from: node),
            items: Self.typedChildren(children),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            style: start.has_value ? .decimal : nil,
            delimiter: start.has_value
                ? (delimiter == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_ONE_PAREN ? .oneParen : .period)
                : nil,
            tight: tight
        )
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
    public var isTask: Bool { marker != nil }
    public var isComplete: Bool { marker != nil && marker != " " }

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
