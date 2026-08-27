import MarkdownCoreC

/// Emphasised text — one `*` or `_` pair.
public struct Emphasis: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The emphasised inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Emphasis` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Emphasis {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        self.init(id: id, scope: Self.scope(from: node), content: Self.children(from: node, owner: id.block))
    }
}
