import MarkdownCoreC

/// A document-owned specimen definition in the citation model.
/// Definitions are ordered by source scope and visited after footnotes. The
/// syntax first lands with P9b; display numbering is derived by consumers.
public struct Specimen: Sendable {
    /// The source range of the definition.
    public var scope: Scope { fields.scope }
    /// The authored label, or `nil` for an anonymous definition.
    public var id: String? { fields.id }
    /// An explicit counter reset, or `nil` when numbering continues.
    public var start: Int64? { fields.start }
    /// The parsed content of the definition.
    public var content: MarkupCollection<any Markup> { MarkupCollection(tree: tree, recordIndices: fields.content) }

    struct Fields: Sendable {
        let scope: Scope
        let id: String?
        let start: Int64?
        let content: [Int]
    }

    let tree: ValueTree
    let index: Int
    private var fields: Fields {
        guard case let .specimen(fields) = tree.records[index] else {
            preconditionFailure("Invalid Specimen record")
        }
        return fields
    }
}

extension Specimen.Fields {
    init(from specimen: OpaquePointer, content: [Int]) {
        var id = markdown_core_optional_string()
        var start = markdown_core_optional_i64()
        precondition(markdown_core_specimen_properties(specimen, &id, &start), "Invalid native specimen")
        self.init(
            scope: Scope(from: markdown_core_specimen_scope(specimen)),
            id: id.string,
            start: start.has_value ? start.value : nil,
            content: content
        )
    }
}
