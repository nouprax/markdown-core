import MarkdownCoreC

public struct FootnoteDefinition: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let id: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer) {
        var id = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &id)
        self.init(
            scope: Self.scope(from: node),
            children: Self.children(from: node),
            id: id.requiredString
        )
    }
}

public struct FootnoteReference: Markup {
    public let scope: Scope
    public let children: [any Markup] = []
    public let id: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer) {
        var id = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &id)
        self.init(scope: Self.scope(from: node), id: id.requiredString)
    }
}
