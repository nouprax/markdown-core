import MarkdownCoreC

public struct FootnoteDefinition: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    public let label: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.identity(of: node)
        var label = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &label)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            label: label.requiredString
        )
    }
}

public struct FootnoteReference: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup] = []
    public let label: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.identity(of: node)
        var label = markdown_core_string_view()
        markdown_core_node_footnote_id(node, &label)
        self.init(id: id, revision: revision, label: label.requiredString)
    }
}
