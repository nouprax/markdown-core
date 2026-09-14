import MarkdownCoreC

/// An inline or leaf directive — `:name[label]{key=value}` or `::name[…]{…}`.
///
/// Requires the `directives` extension. There is no placement mode: an inline
/// directive is always embedded and a ``DirectiveBlock`` always standalone, so
/// the value was implied by the kind.
public struct Directive: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let name: String
        let label: MarkupReference<DirectiveLabel>?
    }

    let fields: Stored<Fields>

    /// Where it is, its leading colon included. See ``Scope``.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The directive's name, without its colons.
    public var name: String { fields.read { $0.name } }
    /// The bracketed label, or `nil` when the source wrote none.
    public var label: DirectiveLabel? { fields.optionalElement { $0.label } }
}

extension Directive.Fields {
    init(from node: OpaquePointer, label: Int?) {
        let values = DirectiveValues(from: node)
        guard let name = values.name else { preconditionFailure("Inline directive requires a name") }
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            name: name,
            label: label.map { .init(index: $0) }
        )
    }
}
