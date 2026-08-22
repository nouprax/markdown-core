import MarkdownCoreC

public struct Directive: Markup {
    public let scope: Scope
    public let name: String
    /// The attributes the source wrote, sorted by name, or `nil` when it wrote
    /// no `{...}` at all. A written-but-empty `{}` is `[]` and stays distinct.
    public let attributes: [DirectiveAttribute]?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            name: values.name,
            attributes: values.attributes,
            label: Self.directiveLabel(from: node)
        )
    }
}
