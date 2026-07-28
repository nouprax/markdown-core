import MarkdownCoreC

/// A container directive block (`:::name[label]{attributes}`).
public struct DirectiveBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's direct children in source order.
    public let children: [any Markup]
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block.
    public let mode: PlacementMode
    /// The directive's name.
    public let name: String
    /// The raw attribute text between the braces, if any.
    public let attributes: String?
    /// The number of leading `children` that form the directive's label;
    /// nil when the directive declares no label.
    public let labelCount: Int?

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        let values = DirectiveValues(from: node)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            labelCount: values.labelCount
        )
    }
}
