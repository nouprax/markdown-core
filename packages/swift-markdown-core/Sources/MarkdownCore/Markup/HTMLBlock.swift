import MarkdownCoreC

/// A raw HTML block.
public struct HTMLBlock: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The HTML exactly as written. Nothing in it is parsed or escaped.
    public let literal: String

    /// Dispatches to the visitor's `HTMLBlock` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTMLBlock {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string()
        markdown_core_node_literal(node, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
