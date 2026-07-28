import MarkdownCoreC

/// The result of one session commit: the new immutable snapshot and the
/// exact difference from the previous revision.
public struct Commit: Sendable {
    /// The new committed snapshot.
    public let document: Document
    /// The exact difference from the previous revision.
    public let delta: Delta
}

/// The id sets of one commit. The four arrays are disjoint: `added` and
/// `removed` list nodes that appeared and disappeared, `changed` lists nodes
/// whose own fields or direct child list changed, and `bubbled` lists
/// ancestors whose revision advanced only because a descendant changed. Ids
/// of removed nodes are retired and never reused. A pure positional shift is
/// not a change and produces no entry.
public struct Delta: Sendable, Hashable {
    /// The revision the session held before this commit.
    public let beforeRevision: UInt64
    /// The revision this commit produced.
    public let afterRevision: UInt64
    /// Nodes that did not exist at the previous revision.
    public let added: [MarkupID]
    /// Nodes that no longer exist; their ids are retired, never reused.
    public let removed: [MarkupID]
    /// Nodes whose own fields or direct child list changed.
    public let changed: [MarkupID]
    /// Ancestors whose revision advanced only because a descendant changed.
    public let bubbled: [MarkupID]
}

extension Delta {
    init(from native: OpaquePointer, lineage: UInt64) {
        var before: UInt64 = 0
        var after: UInt64 = 0
        markdown_core_delta_revisions(native, &before, &after)
        self.init(
            beforeRevision: before,
            afterRevision: after,
            added: Self.ids(lineage) { markdown_core_delta_added(native, &$0) },
            removed: Self.ids(lineage) { markdown_core_delta_removed(native, &$0) },
            changed: Self.ids(lineage) { markdown_core_delta_changed(native, &$0) },
            bubbled: Self.ids(lineage) { markdown_core_delta_bubbled(native, &$0) }
        )
    }

    private static func ids(
        _ lineage: UInt64,
        from accessor: (inout UnsafePointer<UInt64>?) -> Int
    ) -> [MarkupID] {
        var pointer: UnsafePointer<UInt64>?
        let count = accessor(&pointer)
        guard let pointer, count > 0 else { return [] }
        return (0..<count).map { MarkupID(lineage: lineage, rawValue: pointer[$0]) }
    }
}
