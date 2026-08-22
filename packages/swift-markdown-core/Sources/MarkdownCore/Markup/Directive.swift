import MarkdownCoreC

public struct Directive: Markup {
    public let scope: Scope
    public let name: String
    public let attributes: String?
    /// The label's inline content, or `nil` when the source wrote no label.
    /// A written-but-empty label is `[]` and stays distinct from `nil`.
    public let label: [any Markup]?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            name: values.name,
            attributes: values.attributes,
            label: Self.directiveLabel(from: node, count: values.labelCount)
        )
    }
}
