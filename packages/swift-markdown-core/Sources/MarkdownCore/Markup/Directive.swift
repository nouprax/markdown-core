import MarkdownCoreC

/// An inline or leaf directive — `:name[label]{key=value}` or `::name[…]{…}`.
///
/// Requires the `directives` extension. There is no placement mode: an inline
/// directive is always embedded and a ``DirectiveBlock`` always standalone, so
/// the value was implied by the kind.
public struct Directive: Markup {
    /// Where it is, its leading colon included. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The directive's name, without its colons.
    public let name: String
    /// The bracketed label, or `nil` when the source wrote none.
    public let label: DirectiveLabel?

    /// Dispatches to the visitor's `Directive` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Directive {
    init(from node: OpaquePointer, label: DirectiveLabel?) {
        let values = DirectiveValues(from: node)
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            name: values.name,
            label: label
        )
    }
}
