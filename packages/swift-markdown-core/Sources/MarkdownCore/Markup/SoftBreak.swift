import MarkdownCoreC

/// A line ending inside a paragraph that the author did not force.
///
/// A leaf: it has no content, and its scope is all there is to read.
public struct SoftBreak: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope

    /// Dispatches to the visitor's `SoftBreak` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension SoftBreak {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node))
    }
}
