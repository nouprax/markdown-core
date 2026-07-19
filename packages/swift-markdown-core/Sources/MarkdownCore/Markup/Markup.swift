import MarkdownCoreC

public struct Position: Sendable, Hashable {
    public let line: Int32
    public let column: Int32

    public init(line: Int32, column: Int32) {
        self.line = line
        self.column = column
    }
}

public struct Scope: Sendable, Hashable {
    public let start: Position
    public let end: Position

    public init(start: Position, end: Position) {
        self.start = start
        self.end = end
    }
}

/// Session-scoped node identity: `rawValue` is unique within the owning
/// session and never reused; `lineage` is the session's random salt, so nodes
/// from different sessions (including separate one-shot parses) never share
/// an identity. Stable across incremental commits while the node remains the
/// same kind of thing at the same place.
public struct MarkupID: Sendable, Hashable {
    public let lineage: UInt64
    public let rawValue: UInt64

    public init(lineage: UInt64, rawValue: UInt64) {
        self.lineage = lineage
        self.rawValue = rawValue
    }
}

/// A node of the canonical Markdown value tree.
///
/// Nodes are immutable values. Equality and hashing are O(1) and
/// allocation-free: two nodes are equal exactly when they have the same
/// `id` and the same `revision`, which the engine guarantees implies
/// identical AST content (fields and descendants). Absolute source position
/// is not content — resolve it with `Document.scope(of:)` or receive it from
/// `MarkupWalker` events.
public protocol Markup: Sendable, Identifiable, Hashable where ID == MarkupID {
    var id: MarkupID { get }

    /// The commit revision at which this node's own fields, child list, or
    /// any descendant last changed. A pure positional shift caused by an
    /// edit elsewhere never changes a node's revision.
    var revision: UInt64 { get }

    func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result
}

extension Markup {
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.id == rhs.id && lhs.revision == rhs.revision
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
        hasher.combine(revision)
    }
}

public enum PlacementMode: String, Sendable {
    case embedded
    case standalone
}
