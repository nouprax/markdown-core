import MarkdownCoreC

/// A definition-list association preserving its authored collections.
public struct Definition: Markup {
    let record: DefinitionRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The authored source range, including the term and all bodies.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The inline term, visited before the body collections.
    public var term: MarkupCollection<any Markup> { record.collection(0..<record.ends[0]) }
    /// The nonempty ordered collection of block bodies; an individual body may be empty.
    public var content: MarkupGroups<any Markup> { MarkupGroups(owner: record, bounds: record.ends) }
    /// Whether the first body immediately follows its term without a blank line.
    public var compact: Bool { record.compact }
}

/// Children: the term's nodes, then each body's blocks in order. `ends` holds
/// where each of those relations ends, the term's first.
final class DefinitionRecord: MarkupRecord, @unchecked Sendable {
    let compact: Bool
    let ends: [Int]

    init(_ fields: InheritedFields, compact: Bool, term: [MarkupRecord], bodies: [[MarkupRecord]]) {
        self.compact = compact
        var ends = [term.count]
        for body in bodies { ends.append(ends[ends.count - 1] + body.count) }
        self.ends = ends
        super.init(fields, children: term + bodies.joined())
    }

    override var markup: any Markup { Definition(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: DefinitionRecord.self)
        return compact == other.compact && ends == other.ends
    }

    override func relation(at step: Int) -> Relation? {
        guard step < ends.count else { return nil }
        return Relation(
            name: step == 0 ? "DefinitionTerm" : "DefinitionBody",
            indices: (step == 0 ? 0 : ends[step - 1])..<ends[step]
        )
    }
}

extension DefinitionRecord {
    convenience init(from node: OpaquePointer, term: [MarkupRecord], bodies: [[MarkupRecord]]) {
        self.init(
            InheritedFields(from: node),
            compact: markdown_core_node_definition_compact(node),
            term: term,
            bodies: bodies
        )
    }
}

extension Definition: RecordBacked {
    var base: MarkupRecord { record }
}
