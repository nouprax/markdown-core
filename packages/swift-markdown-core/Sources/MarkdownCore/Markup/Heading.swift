import MarkdownCoreC

/// An ATX or setext heading.
///
/// Both spellings produce this one kind, and the node does not record which the
/// author used: `# Title` and `Title` over `=====` are the same heading.
public struct Heading: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The heading's inline content, its `#` markers excluded.
    public let content: [any Markup]
    /// 1 through 6. A `#######` line is not a heading at all.
    public let level: Int32

    /// Dispatches to the visitor's `Heading` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer) {
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(scope: Self.scope(from: node), content: Self.children(from: node), level: level)
    }
}
