import MarkdownCoreC

public enum ListFlavor: String, Sendable {
    case bullet
    case ordered
}

public struct List: Markup {
    public let scope: Scope
    /// A list owns `ListItem`s and nothing else. The contract has said so since
    /// it was written; until Step 15A this was a flat `[any Markup]`, so the
    /// type could not.
    public let items: [ListItem]
    public let flavor: ListFlavor
    public let start: Int64?
    public let tight: Bool

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
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

public struct ListItem: Markup {
    public let scope: Scope
    public let content: [any Markup]
    public let checked: Bool?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
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
