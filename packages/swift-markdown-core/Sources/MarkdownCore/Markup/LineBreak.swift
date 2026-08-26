import MarkdownCoreC

/// A hard line break — a backslash or two or more spaces before the line ending.
///
/// A leaf: it has no content, and its scope is all there is to read.
public struct LineBreak: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope

    /// Dispatches to the visitor's `LineBreak` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension LineBreak {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node))
    }
}
