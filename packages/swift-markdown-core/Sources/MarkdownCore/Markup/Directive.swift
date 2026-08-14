import MarkdownCoreC

/// An inline directive (`:name[label]{attributes}`).
public struct Directive: Markup {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The document revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an
    /// append that only grows this node's extent leaves every reactive
    /// comparison untouched.
    public let scope: Scope
    /// Whether the construct is `embedded` in surrounding inline content or
    /// stands alone as its own block.
    ///
    /// Always `embedded` for inline directives.
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

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        let values = DirectiveValues(from: node)
        let (label, content) = values.partition(builder.children(node))
        precondition(content.isEmpty, "inline directive contains block content")
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            mode: values.mode,
            name: values.name,
            attributes: values.attributes,
            label: label
        )
    }
}
