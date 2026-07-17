import MarkdownCoreC

public struct Text: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup] = []
    public let literal: String

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Text {
    init(from node: OpaquePointer, in decoder: NodeDecoder) {
        let (id, revision) = decoder.identity(of: node)
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, revision: revision, literal: literal.requiredString)
    }
}
