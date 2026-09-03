import MarkdownCoreC

/// A footnote definition.
///
/// `identifier` keeps the leading `^` that `label` does not carry, so a footnote
/// and a link definition of one name cannot collide in a consumer's single map.
public struct FootnoteDefinition: Markup {
    /// The source range, from the opening bracket.
    public let scope: Scope
    /// The definition's block content.
    public let content: [any Markup]
    /// The label as written, delimiters and caret excluded.
    public let label: String
    /// The match key, which keeps the caret.
    public let identifier: String

    /// Dispatches to the visitor's `FootnoteDefinition` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer, content: [any Markup]) {
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            scope: Self.scope(from: node),
            content: content,
            label: label.requiredString,
            identifier: identifier.requiredString
        )
    }
}

/// A footnote call. There is one footnote syntax, so it carries no form.
public struct FootnoteReference: Markup {
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters and caret excluded.
    public let label: String
    /// The match key, which keeps the caret.
    public let identifier: String

    /// Dispatches to the visitor's `FootnoteReference` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer) {
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            scope: Self.scope(from: node),
            label: label.requiredString,
            identifier: identifier.requiredString
        )
    }
}
