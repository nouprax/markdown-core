import MarkdownCoreC

/// Whether a list is bulleted or ordered.
public enum ListFlavor: String, Sendable {
    case bullet
    case ordered
}

/// A bullet or ordered list of `ListItem` children.
public struct List: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// Whether the list is bulleted or ordered.
    public let flavor: ListFlavor
    /// An ordered list's starting number; nil for bullet lists.
    public let start: Int64?
    /// Whether the list renders tight (no paragraph spacing between items).
    public let isTight: Bool

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
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            isTight: tight
        )
    }
}

/// One item of a `List`.
public struct ListItem: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// A task-list item's checkbox state; nil for plain items.
    public let isChecked: Bool?

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
            children: builder.children(node),
            isChecked: checked.has_value ? checked.value : nil
        )
    }
}
