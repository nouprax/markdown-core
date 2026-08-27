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
/// It carries no destination: the destination is stated once, at the
/// definition, and ``definition`` names it — the identity of the
/// ``ReferenceDefinition`` this reference resolved to, the first definition of
/// its label in document order. All three forms resolve identically, so
/// nothing else on the node recovers which one the author wrote.
public struct LinkReference: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The spelling the source used.
    public let form: ReferenceForm
    /// The identity of the definition this reference resolved to.
    public let definition: Identity
    /// The reference's inline content.
    public let content: [any Markup]

    /// Dispatches to the visitor's `LinkReference` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

/// An image reference. As ``LinkReference``; the content is parsed alt text.
public struct ImageReference: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The source range, from the opening bracket to the closing one.
    public let scope: Scope
    /// The label as written, delimiters excluded.
    public let label: String
    /// The spelling the source used.
    public let form: ReferenceForm
    /// The identity of the definition this reference resolved to.
    public let definition: Identity
    /// The parsed alt text.
    public let content: [any Markup]

    /// Dispatches to the visitor's `ImageReference` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
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
        let id = Self.identity(from: node)
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            label: label.requiredString,
            form: referenceForm(from: node),
            definition: referenceDefinition(from: node),
            content: Self.children(from: node)
        )
    }
}

extension ImageReference {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var label = markdown_core_string()
        var identifier = markdown_core_string()
        markdown_core_node_association(node, &label, &identifier)
        self.init(
            id: id,
            scope: Self.scope(from: node),
            label: label.requiredString,
            form: referenceForm(from: node),
            definition: referenceDefinition(from: node),
            content: Self.children(from: node)
        )
    }
}
