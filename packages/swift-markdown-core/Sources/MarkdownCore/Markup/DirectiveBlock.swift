import MarkdownCoreC

public struct DirectiveBlock: Markup {
    public let scope: Scope
    public let name: String
    public let attributes: String?
    /// The label's inline content, or `nil` when the source wrote no label.
    public let label: [any Markup]?
    /// The block content the fence encloses. Distinct from `label`, which the
    /// C tree keeps in the same child list -- see `Markup.directiveLabel`.
    public let content: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            name: values.name,
            attributes: values.attributes,
            label: Self.directiveLabel(from: node, count: values.labelCount),
            content: Self.directiveContent(from: node)
        )
    }
}
