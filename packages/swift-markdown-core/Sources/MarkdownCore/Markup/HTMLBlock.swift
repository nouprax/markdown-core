import MarkdownCoreC

public struct HTMLBlock: Markup {
    public let scope: Scope
    public let children: [any Markup] = []
    public let literal: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTMLBlock {
    init(from node: OpaquePointer) {
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(scope: Self.scope(from: node), literal: literal.requiredString)
    }
}
