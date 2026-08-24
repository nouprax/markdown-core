import MarkdownCoreC

/// Which of the three reference spellings the source wrote.
public enum ReferenceForm: Sendable {
    /// `[text][label]`.
    case full
    /// `[label][]`.
    case collapsed
    /// `[label]`.
    case shortcut
}

/// A link reference.
///
/// It carries no destination: the destination is stated once, at the definition,
/// and `identifier` is what names it. All three forms resolve identically, so
/// nothing else on the node recovers which one the author wrote.
public struct LinkReference: Markup {
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The match key. Compare it by bytes, never by `String ==`.
    public let identifier: String
    /// The spelling the source used.
    public let form: ReferenceForm
    /// The reference's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `LinkReference` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

/// An image reference. As ``LinkReference``; the content is parsed alt text.
public struct ImageReference: Markup {
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The match key. Compare it by bytes, never by `String ==`.
    public let identifier: String
    /// The spelling the source used.
    public let form: ReferenceForm
    /// The parsed alt text.
    public let content: [any Markup]

    /// Dispatches to the visitor's `ImageReference` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

func referenceForm(from node: OpaquePointer) -> ReferenceForm {
    var form = MARKDOWN_CORE_REFERENCE_SHORTCUT
    markdown_core_node_reference_form(node, &form)
    switch form {
    case MARKDOWN_CORE_REFERENCE_FULL: return .full
    case MARKDOWN_CORE_REFERENCE_COLLAPSED: return .collapsed
    default: return .shortcut
    }
}

extension LinkReference {
    init(from node: OpaquePointer) {
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            scope: Self.scope(from: node),
            label: label.requiredString,
            identifier: identifier.requiredString,
            form: referenceForm(from: node),
            content: Self.children(from: node)
        )
    }
}

extension ImageReference {
    init(from node: OpaquePointer) {
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            scope: Self.scope(from: node),
            label: label.requiredString,
            identifier: identifier.requiredString,
            form: referenceForm(from: node),
            content: Self.children(from: node)
        )
    }
}
