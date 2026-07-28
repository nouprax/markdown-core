import MarkdownCoreC

/// Struck-through inline content (the `~~` extension).
public struct Strikethrough: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Strikethrough {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        self.init(id: id, revision: revision, children: builder.children(node))
    }
}
