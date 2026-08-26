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

/// A bulleted or numbered list.
public struct List: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// A list owns `ListItem`s and nothing else. The contract has said so since
    /// it was written; until Step 15A this was a flat `[any Markup]`, so the
    /// type could not.
    public let items: [ListItem]
    /// Bulleted or numbered.
    public let flavor: ListFlavor
    /// The first number an ordered list counts from, and `nil` for a bulleted
    /// one — which is the only reason it is optional.
    public let start: Int64?
    /// Whether the source separated the items by blank lines. A loose list
    /// wraps each item's text in a ``Paragraph``; a tight one does not, so
    /// this is already visible in the tree and is stated here as well.
    public let tight: Bool

    /// Dispatches to the visitor's `List` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension List {
    init(from node: OpaquePointer) {
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &tight)
        self.init(
            scope: Self.scope(from: node),
            items: Self.typedChildren(from: node),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
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
    /// `nil` when the item is not a task item at all. `[ ]` is `false` and
    /// `[x]` is `true`, so all three states are distinct. Requires the
    /// `taskLists` extension.
    public let checked: Bool?

    /// Dispatches to the visitor's `ListItem` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ListItem {
    init(from node: OpaquePointer) {
        var checked = markdown_core_optional_bool()
        markdown_core_node_list_item_checked(node, &checked)
        self.init(
            scope: Self.scope(from: node),
            content: Self.children(from: node),
            checked: checked.has_value ? checked.value : nil
        )
    }
}
