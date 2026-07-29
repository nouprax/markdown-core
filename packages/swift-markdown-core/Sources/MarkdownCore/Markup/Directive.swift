import MarkdownCoreC

/// An inline directive (`:name[label]{attributes}`).
public struct Directive: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block; always `embedded` for inline
    /// directives.
    public let mode: PlacementMode
    /// The directive's name.
    public let name: String
    /// The raw attribute text between the braces, if any.
    public let attributes: String?
    /// The directive's label; nil when the directive declares no label —
    /// distinct from an explicit empty `[]`.
    public let label: DirectiveLabel?

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        let values = DirectiveValues(from: node)
        let (label, content) = values.partition(builder.children(node))
        precondition(content.isEmpty, "inline directive contains block content")
        self.init(
            id: id,
            revision: revision,
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            label: label
        )
    }
}
