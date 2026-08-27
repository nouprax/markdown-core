import MarkdownCoreC

/// A paragraph.
public struct Paragraph: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The paragraph's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Paragraph` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Paragraph {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        self.init(id: id, scope: Self.scope(from: node), content: Self.children(from: node, owner: id.block))
    }
}
