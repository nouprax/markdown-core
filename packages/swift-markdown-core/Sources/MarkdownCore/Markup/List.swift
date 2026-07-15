import MarkdownCoreC

public enum ListFlavor: String, Sendable {
    case bullet
    case ordered
}

public struct List: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let flavor: ListFlavor
    public let start: Int64?
    public let isTight: Bool

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
            children: Self.children(from: node),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            isTight: tight
        )
    }
}

public struct ListItem: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let isChecked: Bool?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ListItem {
    init(from node: OpaquePointer) {
        var checked = markdown_core_optional_bool()
        markdown_core_node_list_item_checked(node, &checked)
        self.init(
            scope: Self.scope(from: node),
            children: Self.children(from: node),
            isChecked: checked.has_value ? checked.value : nil
        )
    }
}
