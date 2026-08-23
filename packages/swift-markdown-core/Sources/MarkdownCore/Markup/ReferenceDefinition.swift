import MarkdownCoreC

/// A link reference definition, at the byte where its opening bracket was written.
///
/// `label` is the bytes between the brackets exactly as the source spells them:
/// character escapes and character references unresolved, whitespace uncollapsed,
/// case unfolded. `destination` is never absent, because a definition that could
/// not build one is not produced at all. `title` is `nil` when the source wrote
/// none and empty when it wrote an empty one.
public struct ReferenceDefinition: Markup {
    /// The source range the definition covers, from its opening bracket.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The destination the label resolves to.
    public let destination: String
    /// The title, or `nil` when the source wrote none.
    public let title: String?

    /// Dispatches to the visitor's `ReferenceDefinition` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ReferenceDefinition {
    init(from node: OpaquePointer) {
        var label = markdown_core_string_view()
        var destination = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_definition_properties(node, &label, &destination, &title)
        self.init(
            scope: Self.scope(from: node),
            label: label.requiredString,
            destination: destination.requiredString,
            title: title.optionalString
        )
    }
}
