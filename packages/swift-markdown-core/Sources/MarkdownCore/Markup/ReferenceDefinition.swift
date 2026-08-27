import MarkdownCoreC

/// A link reference definition, at the byte where its opening bracket was written.
///
/// `label` is the bytes between the brackets exactly as the source spells them.
/// `norm` is the match key — full Unicode case fold, trimmed, internal
/// whitespace collapsed — and neither derives the other. `destination` is never
/// absent, because a definition that could not build one is not produced at all.
/// `title` is `nil` when the source wrote none and empty when it wrote an empty one.
public struct ReferenceDefinition: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The source range the definition covers, from its opening bracket.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The match key. Compare it by bytes, never by `String ==`.
    public let norm: String
    /// The destination the label resolves to.
    public let destination: String
    /// The title, or `nil` when the source wrote none.
    public let title: String?

    /// Dispatches to the visitor's `ReferenceDefinition` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ReferenceDefinition {
    init(from node: OpaquePointer, owner: UInt32) {
        let id = Self.identity(from: node, owner: owner)
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        var destination = markdown_core_string()
        var title = markdown_core_optional_string()
        markdown_core_node_association(node, &label, &identifier)
        markdown_core_node_definition_resource(node, &destination, &title)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            label: label.requiredString,
            norm: identifier.requiredString,
            destination: destination.requiredString,
            title: title.string
        )
    }
}
