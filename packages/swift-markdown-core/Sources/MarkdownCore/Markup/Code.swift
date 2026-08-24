import MarkdownCoreC

/// An inline code span.
public struct Code: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The span's content. Its backticks are in no literal anywhere.
    public let literal: String

    /// Dispatches to the visitor's `Code` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Code {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
