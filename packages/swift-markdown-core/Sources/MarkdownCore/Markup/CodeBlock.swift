import MarkdownCoreC

public struct CodeBlock: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup] = []
    public let info: String?
    public let language: String?
    public let literal: String
    public let isFenced: Bool
    public let isClosed: Bool

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CodeBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var info = markdown_core_string_view()
        var language = markdown_core_string_view()
        var literal = markdown_core_string_view()
        var fenced = false
        var closed = false
        markdown_core_node_code_block_properties(
            node,
            &info,
            &language,
            &literal,
            &fenced,
            &closed
        )
        self.init(
            id: id,
            revision: revision,
            info: info.optionalString,
            language: language.optionalString,
            literal: literal.requiredString,
            isFenced: fenced,
            isClosed: closed
        )
    }
}
