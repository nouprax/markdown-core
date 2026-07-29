import MarkdownCoreC

/// Whether a list is bulleted or ordered.
public enum ListFlavor: String, Sendable {
    case bullet
    case ordered
}

/// A bullet or ordered list of `ListItem` values.
public struct List: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Whether the list is bulleted or ordered.
    public let flavor: ListFlavor
    /// An ordered list's starting number; nil for bullet lists.
    public let start: Int64?
    /// Whether the list renders tight (no paragraph spacing between items).
    public let tight: Bool
    /// The list's items in source order.
    public let items: [ListItem]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension List {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var tight = false
        markdown_core_node_list_properties(node, &flavor, &start, &tight)
        let items = builder.children(node).map { child -> ListItem in
            guard let item = child as? ListItem else {
                preconditionFailure("list contains a non-item node")
            }
            return item
        }
        self.init(
            id: id,
            revision: revision,
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            tight: tight,
            items: items
        )
    }
}

/// One item of a `List`.
public struct ListItem: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// A task-list item's checkbox state; nil for plain items.
    public let checked: Bool?
    /// The item's block content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ListItem {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var checked = markdown_core_optional_bool()
        markdown_core_node_list_item_checked(node, &checked)
        self.init(
            id: id,
            revision: revision,
            checked: checked.has_value ? checked.value : nil,
            content: builder.children(node)
        )
    }
}
