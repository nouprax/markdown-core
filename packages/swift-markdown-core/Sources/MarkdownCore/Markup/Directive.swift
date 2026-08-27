import MarkdownCoreC

/// An inline or leaf directive — `:name[label]{key=value}` or `::name[…]{…}`.
///
/// Requires the `directives` extension. There is no placement mode: an inline
/// directive is always embedded and a ``DirectiveBlock`` always standalone, so
/// the value was implied by the kind.
public struct Directive: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is, its leading colon included. See ``Scope``.
    public let scope: Scope
    /// The directive's name, without its colons.
    public let name: String
    /// The attributes the source wrote, sorted by name, or `nil` when it wrote
    /// no `{...}` at all. A written-but-empty `{}` is `[]` and stays distinct.
    public let attributes: [DirectiveAttribute]?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?

    /// Dispatches to the visitor's `Directive` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        let values = DirectiveValues(from: node)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            name: values.name,
            attributes: values.attributes,
            label: Self.directiveLabel(from: node, owner: id.block)
        )
    }
}
