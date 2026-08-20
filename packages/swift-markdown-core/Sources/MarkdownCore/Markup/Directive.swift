import MarkdownCoreC

public struct Directive: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let mode: PlacementMode
    public let name: String
    public let attributes: String?
    public let labelCount: Int?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            children: Self.children(from: node),
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            labelCount: values.labelCount
        )
    }
}
