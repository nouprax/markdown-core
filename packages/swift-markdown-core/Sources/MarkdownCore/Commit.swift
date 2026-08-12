import MarkdownCoreC

/// What ``Document/edit(_:)`` returns: the document the new text describes,
/// and what changed on the way there.
public struct Commit: Sendable {
    /// The new document. The one it was produced from is untouched.
    public let document: Document
    /// What differs between the two.
    public let delta: Delta
}

/// Which parts of a node's projection differ.
///
/// There is no lifecycle flag. A node that no longer exists has no parts, so
/// its set is EMPTY, and that is how a consumer tells a deletion from a
/// change. A node that did not exist before differs from absence in every
/// part it has, so it carries all of them.
public struct DiffParts: OptionSet, Sendable, Hashable {
    /// The set's raw bit pattern.
    public let rawValue: UInt32
    /// Creates a part set from its raw bit pattern.
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// One of the node's own fields differs, string-valued ones included.
    ///
    /// A link's destination and title, a reference's label, a directive's
    /// name and attributes are all `value`, not `text`. Never a kind change —
    /// paired nodes always share a kind, because a changed kind is a
    /// retirement and a creation rather than a difference.
    public static let value = DiffParts(rawValue: 1 << 0)
    /// The node's canonical text differs.
    public static let text = DiffParts(rawValue: 1 << 1)
    /// The node's own child list differs.
    public static let children = DiffParts(rawValue: 1 << 2)
    /// Something below the node differs.
    ///
    /// Reported together with whatever of the node's own differs, never
    /// instead of it. Every node above a change carries some part, so a
    /// subtree whose root the delta does not name holds no change at all.
    public static let descendant = DiffParts(rawValue: 1 << 3)

    /// Whether this entry reports a node that no longer exists.
    public var isRetired: Bool { isEmpty }
}

/// One node whose projection differs between two documents.
public struct Diff: Sendable, Hashable {
    /// Which node.
    public let markup: MarkupID
    /// What about it differs; empty means it no longer exists.
    public let parts: DiffParts
}

/// The difference between two documents: one list, in order.
///
/// One list and not four id sets. The four — added, removed, changed,
/// bubbled — could not say WHAT about a node changed, so a consumer that got
/// an id had to re-read the whole node to find out; and three of them were
/// invariably merged back into one ordered sequence by every binding that
/// consumed them, which is the shape they should have had.
public struct Delta: Sendable, Hashable {
    /// The revision the document held before the edit.
    public let beforeRevision: UInt64
    /// The revision the edit produced.
    public let afterRevision: UInt64
    /// Every differing node, in order.
    ///
    /// The order is the new document's postorder over the nodes it still
    /// contains — a node always follows its own children — with each retired
    /// subtree spliced in preorder where it was found, ahead of its surviving
    /// parent's row. A consumer rebuilding immutable values bottom-up
    /// therefore reads the list once, front to back: every child it needs is
    /// built by the time it reaches the parent, and a parent's dead children
    /// are gone before it re-reads that child list.
    public let diffs: [Diff]
}

extension Delta {
    init(from native: OpaquePointer, series: UInt64) {
        var before: UInt64 = 0
        var after: UInt64 = 0
        markdown_core_delta_revisions(native, &before, &after)
        var rows: UnsafePointer<markdown_core_diff>?
        let count = markdown_core_delta_diffs(native, &rows)
        var diffs: [Diff] = []
        if let rows, count > 0 {
            diffs.reserveCapacity(count)
            for index in 0..<count {
                diffs.append(
                    Diff(
                        markup: MarkupID(series: series, rawValue: rows[index].markup),
                        parts: DiffParts(rawValue: rows[index].parts)
                    )
                )
            }
        }
        self.init(beforeRevision: before, afterRevision: after, diffs: diffs)
    }
}
