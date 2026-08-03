import MarkdownCoreC

/// How a reference was written. The three resolve identically and differ only
/// in source form, which the tree keeps because it is what was written.
public enum ReferenceForm: Sendable, Hashable {
    case full
    case collapsed
    case shortcut
}

/// A link reference definition, at the position it was written. The
/// destination is stated here, once, rather than copied into every reference
/// that resolves to it.
public struct ReferenceDefinition: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The label between `[` and `]`, exactly as written.
    public let label: String
    /// The destination this definition assigns to its label.
    public let destination: String?
    /// The optional title.
    public let title: String?

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

/// `[text][label]`, `[label][]`, or `[label]`. It carries no destination:
/// which definition the label resolves to is an answer, asked of the document
/// rather than read off the node. ``Link`` stays the inline form `[a](/u)`,
/// whose destination is written in the source.
public struct LinkReference: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The label this reference resolves by, exactly as written.
    public let label: String
    /// The source form the reference was written in.
    public let form: ReferenceForm
    /// The reference's inline caption content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

/// `![alt][label]` and its collapsed and shortcut forms.
public struct ImageReference: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The label this reference resolves by, exactly as written.
    public let label: String
    /// The source form the reference was written in.
    public let form: ReferenceForm
    /// The reference's alternative-text content in source order.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension ReferenceForm {
    init(_ native: markdown_core_reference_form) {
        switch native {
        case MARKDOWN_CORE_REFERENCE_FULL: self = .full
        case MARKDOWN_CORE_REFERENCE_COLLAPSED: self = .collapsed
        default: self = .shortcut
        }
    }
}

extension ReferenceDefinition {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var label = markdown_core_string_view()
        var destination = markdown_core_string_view()
        var title = markdown_core_string_view()
        markdown_core_node_reference_definition_properties(node, &label, &destination, &title)
        self.init(
            id: id,
            revision: revision,
            label: label.requiredString,
            destination: destination.optionalString,
            title: title.optionalString
        )
    }
}

extension LinkReference {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var label = markdown_core_string_view()
        var form = MARKDOWN_CORE_REFERENCE_SHORTCUT
        markdown_core_node_reference_properties(node, &label, &form)
        self.init(
            id: id,
            revision: revision,
            label: label.requiredString,
            form: ReferenceForm(form),
            content: builder.children(node)
        )
    }
}

extension ImageReference {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var label = markdown_core_string_view()
        var form = MARKDOWN_CORE_REFERENCE_SHORTCUT
        markdown_core_node_reference_properties(node, &label, &form)
        self.init(
            id: id,
            revision: revision,
            label: label.requiredString,
            form: ReferenceForm(form),
            content: builder.children(node)
        )
    }
}
