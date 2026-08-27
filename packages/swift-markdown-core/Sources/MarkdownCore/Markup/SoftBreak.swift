import MarkdownCoreC

/// A line ending inside a paragraph that the author did not force.
///
/// A leaf: it has no content, and its scope is all there is to read.
public struct SoftBreak: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope

    /// Dispatches to the visitor's `SoftBreak` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension SoftBreak {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        self.init(id: id, scope: Self.scope(from: node))
    }
}
