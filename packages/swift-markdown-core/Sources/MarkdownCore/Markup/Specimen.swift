import MarkdownCoreC

/// A document-owned specimen definition in the citation model.
/// Definitions are ordered by source scope and visited after footnotes. The
/// syntax first lands with P9b; display numbering is derived by consumers.
public struct Specimen: Sendable {
    /// The source range of the definition.
    public let scope: Scope
    /// The authored label, or `nil` for an anonymous definition.
    public let id: String?
    /// An explicit counter reset, or `nil` when numbering continues.
    public let start: Int64?
    /// The parsed content of the definition.
    public let content: [any Markup]
}

extension Specimen {
    init(from specimen: OpaquePointer, content: [any Markup]) {
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
