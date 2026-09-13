import MarkdownCoreC

/// A document-owned specimen definition in the citation model.
/// Definitions are ordered by source scope and visited after footnotes. The
/// syntax first lands with P9b; display numbering is derived by consumers.
public struct Specimen: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let id: String?
        let start: Int64?
        let content: MarkupReferences<any Markup>
    }

    let fields: Stored<Fields>

    /// The source range of the definition.
    public var scope: Scope { fields.scope }
    /// The optional anchor attached to this node.
    public var anchor: String? { fields.anchor }
    /// The ordered attributes attached to this node.
    public var attributes: Attributes { fields.attributes }
    /// The authored label, or `nil` for an anonymous definition.
    public var id: String? { fields.id }
    /// An explicit counter reset, or `nil` when numbering continues.
    public var start: Int64? { fields.start }
    /// The parsed content of the definition.
    public var content: MarkupCollection<any Markup> { fields.content }

    /// Dispatches this node to its typed visitor method.
    public func accept<V: MarkupVisitor>(_ visitor: inout V, phase: MarkupVisitPhase) -> V.Result {
        visitor.visit(self, phase: phase)
    }
}

extension Specimen.Fields {
    init(from specimen: OpaquePointer, content: [Int]) {
        var id = markdown_core_optional_string()
        var start = markdown_core_optional_i64()
        precondition(markdown_core_specimen_properties(specimen, &id, &start), "Invalid native specimen")
        self.init(
            scope: Scope(from: markdown_core_node_scope(specimen)),
            anchor: markdown_core_node_anchor(specimen).string,
            attributes: Attributes(from: specimen),
            id: id.string,
            start: start.has_value ? start.value : nil,
            content: .init(indices: content)
        )
    }
}
