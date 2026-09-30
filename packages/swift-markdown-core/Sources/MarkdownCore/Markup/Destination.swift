import MarkdownCoreC

/// The target of a ``Link`` or ``Embedded``: a tagged value, not a node, so it
/// has no id, no extent and no children, and a branch's fields exist only in that
/// branch.
public enum Destination: Sendable, Hashable {
    /// The complete semantic destination the inherited grammar produced: the
    /// bytes between angle brackets or the bare destination, with backslash
    /// escapes and character references decoded and no percent-encoding,
    /// normalization, or resolution. `[a]()` and `[a](<>)` wrote one and wrote
    /// nothing in it, so they answer `.url("")`. Every link and image owns
    /// this branch.
    case url(String)
    /// The workspace address a cross link produces: a path that may be empty
    /// when an anchor addresses the current document, and the anchor or `nil`.
    case cross(path: String, anchor: String?)
}

/// The destination and title a link or image reads through its resource.
///
/// Every occurrence of one reference definition shares one resource in the C
/// tree, and its identity keys one materialization here, so a long destination
/// referenced many times is decoded once however often it is named.
struct SharedResource {
    let dest: Destination
    let title: String?
    let anchor: String?
    let attributes: Attributes

    static func shared(
        by node: OpaquePointer,
        in resources: inout [UnsafeRawPointer: SharedResource]
    ) -> SharedResource {
        guard let identity = markdown_core_node_resource(node) else {
            preconditionFailure("native link or image has no resource")
        }
        let key = UnsafeRawPointer(identity)
        if let known = resources[key] { return known }
        var title = markdown_core_optional_string()
        markdown_core_node_title(node, &title)
        let inherited = markdown_core_node_inherited_attributes(node)
        let resource = SharedResource(
            dest: Destination(from: node),
            title: title.string,
            anchor: markdown_core_attribute_value_anchor(inherited).string,
            attributes: Attributes(from: inherited)
        )
        resources[key] = resource
        return resource
    }

    /// An occurrence's inherited fields: its own anchor before the resource's,
    /// and the resource's classes and records before its own.
    func fields(of node: OpaquePointer) -> InheritedFields {
        InheritedFields(
            id: MarkupID(markdown_core_node_id(node)),
            extent: Extent(markdown_core_node_extent(node)),
            anchor: markdown_core_attribute_value_anchor(markdown_core_node_primary_attributes(node)).string
                ?? anchor,
            attributes: Attributes(from: node).inheriting(attributes)
        )
    }
}

extension Destination {
    init(from node: OpaquePointer) {
        var destination = markdown_core_destination()
        markdown_core_node_destination(node, &destination)
        switch destination.kind {
        case MARKDOWN_CORE_DESTINATION_CROSS:
            self = .cross(path: destination.path.required, anchor: destination.anchor.string)
        default:
            self = .url(destination.url.required)
        }
    }
}
