import MarkdownCoreC

/// A footnote definition.
///
/// `norm` is the match key the label folds to, and it keeps the leading `^`
/// that `label` does not carry, so a footnote and a link definition of one
/// name cannot collide in a consumer's single map.
public struct FootnoteDefinition: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The source range, from the opening bracket.
    public let scope: Scope
    /// The definition's block content.
    public let content: [any Markup]
    /// The label as written, delimiters and caret excluded.
    public let label: String
    /// The match key, which keeps the caret. Compare it by bytes.
    public let norm: String

    /// Dispatches to the visitor's `FootnoteDefinition` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteDefinition {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            content: Self.children(from: node),
            label: label.requiredString,
            norm: identifier.requiredString
        )
    }
}

/// A footnote call. There is one footnote syntax, so it carries no form.
/// ``definition`` is the identity of the ``FootnoteDefinition`` it resolved
/// to — the first definition of its label in document order.
public struct FootnoteReference: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters and caret excluded.
    public let label: String
    /// The identity of the definition this call resolved to.
    public let definition: Identity

    /// Dispatches to the visitor's `FootnoteReference` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension FootnoteReference {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            label: label.requiredString,
            definition: referenceDefinition(from: node)
        )
    }
}
