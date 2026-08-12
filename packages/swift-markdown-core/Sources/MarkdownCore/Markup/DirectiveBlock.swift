import MarkdownCoreC

/// A container directive block (`:::name[label]{attributes}`).
public struct DirectiveBlock: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block.
    ///
    /// Always `standalone` for directive blocks.
    public let mode: PlacementMode
    /// The directive's name.
    public let name: String
    /// The directive's attribute map in the grammar's own terms, or nil when
    /// no `{...}` container was written.
    ///
    /// An empty container is an empty map.
    public let attributes: [String: String]?
    /// The directive's label; nil when the directive declares no label —
    /// distinct from an explicit empty `[]`.
    public let label: DirectiveLabel?
    /// The directive's block content in source order, label excluded.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        let values = DirectiveValues(from: node)
        let (label, content) = values.partition(builder.children(node))
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            label: label,
            content: content
        )
    }
}
