import MarkdownCoreC

/// A container directive — `:::name[label]{key=value}` and its closing fence.
///
/// Requires the `directives` extension. A malformed label or attribute block
/// leaves the directive standing and the punctuation as prose rather than
/// failing the parse; a diagnostic says so.
public struct DirectiveBlock: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// Where it is, opening fence through closing fence. See ``Scope``.
    public let scope: Scope
    /// The directive's name, without its colons.
    public let name: String
    /// The attributes the source wrote, sorted by name, or `nil` when it wrote
    /// no `{...}` at all.
    public let attributes: [DirectiveAttribute]?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?
    /// The block content the fence encloses.
    public let content: [any Markup]

    /// Dispatches to the visitor's `DirectiveBlock` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            name: DirectiveValues(from: node).name,
            attributes: DirectiveValues(from: node).attributes,
            label: Self.directiveLabel(from: node),
            content: Self.directiveContent(from: node)
        )
    }
}
