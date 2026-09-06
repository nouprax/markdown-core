import MarkdownCoreC

/// An inline link — `[text](destination)`.
///
/// A link written in one of the three reference forms is a ``LinkReference``
/// instead, and carries no destination at all.
public struct Link: Markup {
    /// Where it is, brackets and parentheses included. See ``Scope``.
    public let scope: Scope
    /// The link text, as inline content.
    public let content: [any Markup]
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `.url("")`. A link with no destination at all is
    /// a ``LinkReference``.
    public let dest: Destination
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    public let title: String?

    /// Dispatches to the visitor's `Link` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer, content: [any Markup]) {
        var title = markdown_core_optional_string()
        markdown_core_node_title(node, &title)
        self.init(
            scope: Self.scope(from: node),
            content: content,
            dest: Destination(from: node),
            title: title.string
        )
    }
}
