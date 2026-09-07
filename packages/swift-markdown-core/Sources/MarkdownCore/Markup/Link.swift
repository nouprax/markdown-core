import MarkdownCoreC

/// A link — `[text](destination)`, any of the three reference forms, or an
/// autolink.
///
/// A reference occurrence is the link its definition names: it answers the
/// definition's destination and title and keeps its own scope.
public struct Link: Markup {
    /// Where it is, brackets and parentheses included. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The link text, as inline content.
    public let content: [any Markup]
    /// Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing
    /// in it, so they answer `.url("")`; a reference occurrence answers the
    /// destination its definition stated.
    public let dest: Destination
    /// Optional: `[a](/u)` wrote no title and `[a](/u "")` wrote an empty one.
    public let title: String?

    /// Dispatches to the visitor's `Link` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Link {
    init(from node: OpaquePointer, content: [any Markup], resources: inout [UnsafeRawPointer: SharedResource]) {
        let resource = SharedResource.shared(by: node, in: &resources)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            dest: resource.dest,
            title: resource.title
        )
    }
}
