import MarkdownCoreC

/// A hard line break — a backslash or two or more spaces before the line ending.
///
/// A leaf: it has no content, and its scope is all there is to read.
public struct LineBreak: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes

    /// Dispatches to the visitor's `LineBreak` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension LineBreak {
    init(from node: OpaquePointer) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}
