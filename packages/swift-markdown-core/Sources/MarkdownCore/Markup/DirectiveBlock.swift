import MarkdownCoreC

public struct DirectiveBlock: Markup {
    public let scope: Scope
    public let name: String
    /// The attributes the source wrote, sorted by name, or `nil` when it wrote
    /// no `{...}` at all.
    public let attributes: [DirectiveAttribute]?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?
    /// The block content the fence encloses.
    public let content: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer) {
        self.init(
            scope: Self.scope(from: node),
            name: DirectiveValues(from: node).name,
            attributes: DirectiveValues(from: node).attributes,
            label: Self.directiveLabel(from: node),
            content: Self.directiveContent(from: node)
        )
    }
}
