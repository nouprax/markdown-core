import MarkdownCoreC

/// The target of a ``Link``, ``Embedded`` or ``Reference``: a tagged value, not
/// a node, so it has no id, no extent and no children, and a branch's fields
/// exist only in that branch.
public enum Destination: Sendable, Hashable {
    /// The complete semantic destination the inherited grammar produced: the
    /// bytes between angle brackets or the bare destination, with backslash
    /// escapes and character references decoded and no percent-encoding,
    /// normalization, or resolution. `[a]()` and `[a](<>)` wrote one and wrote
    /// nothing in it, so they answer `.url("")`. A direct link or image and
    /// every reference definition own this branch.
    case url(String)
    /// The workspace address a cross link produces: a path that may be empty
    /// when an anchor addresses the current document, and the anchor or `nil`.
    case cross(path: String, anchor: String?)
    /// The normalized label a reference occurrence — `[t][l]`, `[l][]` or
    /// `[l]` — names. ``Document/reference(for:)`` finds the node it resolves
    /// to.
    case reference(label: String)
}

extension Destination {
    init(from node: OpaquePointer) {
        let destination = answer(markdown_core_destination()) { markdown_core_node_destination(node, $0) }
        switch destination.kind {
        case MARKDOWN_CORE_DESTINATION_URL:
            self = .url(destination.url.required)
        case MARKDOWN_CORE_DESTINATION_CROSS:
            self = .cross(path: destination.path.required, anchor: destination.anchor.string)
        // A C enum switch is never exhaustive in Swift; the one kind
        // left is MARKDOWN_CORE_DESTINATION_REFERENCE.
        default:
            self = .reference(label: destination.label.required)
        }
    }
}
