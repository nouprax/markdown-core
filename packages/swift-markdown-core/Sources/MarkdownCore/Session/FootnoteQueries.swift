import MarkdownCoreC

/// Session-answered footnote presentation state for one node.
///
/// The tree is source-faithful: definitions stay at their source position
/// whether referenced or not, and references always carry their label.
/// Numbering, first-use order, resolution state, and back-reference ordinals
/// are queries against the session's committed revision. When a commit
/// changes only these answers, the affected nodes are reported `changed`
/// with a revision bump and identical dump content.
public struct FootnoteInfo: Sendable, Hashable {
    /// The label's winning definition (for a definition: its own id unless
    /// an earlier definition shadows it); nil while the label is unresolved.
    public let definition: MarkupID?

    /// The label's 1-based first-use ordinal; nil while the label is
    /// unresolved or unreferenced.
    public let number: Int?

    /// For a reference: its 1-based position among the label's references in
    /// document order. nil for definitions.
    public let referenceOrdinal: Int?

    /// How many references share the label.
    public let referenceCount: Int
}

extension MarkupSession {
    /// Answers for the footnote reference or definition with `id` at the
    /// committed revision; nil when `id` does not name a footnote node of
    /// this session.
    public func footnote(of id: MarkupID) -> FootnoteInfo? {
        guard id.lineage == lineage else { return nil }
        var info = markdown_core_footnote_info()
        guard markdown_core_session_footnote_info(session, id.rawValue, &info) else {
            return nil
        }
        return FootnoteInfo(
            definition: info.definition == 0
                ? nil : MarkupID(lineage: lineage, rawValue: info.definition),
            number: info.number == 0 ? nil : Int(info.number),
            referenceOrdinal: info.reference_ordinal == 0 ? nil : Int(info.reference_ordinal),
            referenceCount: Int(info.reference_count)
        )
    }

    /// The referenced (winning) definitions in first-use order — the order a
    /// renderer lists them in.
    public func footnotes() -> [FootnoteDefinition] {
        var ids: UnsafePointer<UInt64>?
        let count = markdown_core_session_footnotes(session, &ids)
        guard let ids, count > 0 else { return [] }
        return (0..<count).map { index in
            guard let definition = mirror[ids[index]] as? FootnoteDefinition else {
                preconditionFailure("footnote index names a non-definition node")
            }
            return definition
        }
    }

    /// The references that resolve to `definition`, in document order — the
    /// renderer's back-reference targets. Empty unless `definition` is a
    /// referenced winning definition of this session.
    public func references(of definition: MarkupID) -> [FootnoteReference] {
        guard definition.lineage == lineage else { return [] }
        var ids: UnsafePointer<UInt64>?
        let count = markdown_core_session_footnote_references(session, definition.rawValue, &ids)
        guard let ids, count > 0 else { return [] }
        return (0..<count).map { index in
            guard let reference = mirror[ids[index]] as? FootnoteReference else {
                preconditionFailure("footnote index names a non-reference node")
            }
            return reference
        }
    }
}
