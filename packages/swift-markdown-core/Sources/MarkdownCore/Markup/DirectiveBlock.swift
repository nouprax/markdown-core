import MarkdownCoreC

/// A container directive — `:::name[label]{key=value}` and its closing fence.
///
/// Requires the `directives` extension. A malformed label or attribute block
/// leaves the directive standing and the punctuation as prose rather than
/// failing the parse.
public struct DirectiveBlock: Markup {
    /// Where it is, opening fence through closing fence. See ``Scope``.
    public let scope: Scope
    /// The directive's name, without its colons.
    public let name: String
    /// The attributes the source wrote, in first-occurrence source order, or
    /// `nil` when it wrote no `{...}` at all.
    public let attributes: [DirectiveAttribute]?
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?
    /// The block content the fence encloses.
    public let content: [any Markup]

    /// Dispatches to the visitor's `DirectiveBlock` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DirectiveBlock {
    init(from node: OpaquePointer, label: DirectiveLabel?, content: [any Markup]) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            name: values.name,
            attributes: values.attributes,
            label: label,
            content: content
        )
    }
}
