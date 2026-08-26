import MarkdownCoreC

/// A run of raw inline HTML.
public struct HTML: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The HTML exactly as written. Nothing in it is parsed or escaped.
    public let literal: String

    /// Dispatches to the visitor's `HTML` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTML {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
