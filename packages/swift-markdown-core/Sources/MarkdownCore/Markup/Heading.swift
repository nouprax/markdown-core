import MarkdownCoreC

/// An ATX or setext heading.
///
/// Both spellings produce this one kind, and the node does not record which the
/// author used: `# Title` and `Title` over `=====` are the same heading.
public struct Heading: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The heading's inline content, its `#` markers excluded.
    public let content: [any Markup]
    /// 1 through 6. A `#######` line is not a heading at all.
    public let level: Int32

    /// Dispatches to the visitor's `Heading` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer, content: [any Markup]) {
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            level: level
        )
    }
}
