import MarkdownCoreC

/// The target of a ``Link`` or ``Image``: a tagged value, not a node, so it
/// has no scope and no children, and a branch's fields exist only in that
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

extension Destination {
    init(from node: OpaquePointer) {
        var destination = markdown_core_destination()
        markdown_core_node_destination(node, &destination)
        switch destination.kind {
        case MARKDOWN_CORE_DESTINATION_CROSS:
            self = .cross(path: destination.path.requiredString, anchor: destination.anchor.string)
        default:
            self = .url(destination.url.requiredString)
        }
    }
}
