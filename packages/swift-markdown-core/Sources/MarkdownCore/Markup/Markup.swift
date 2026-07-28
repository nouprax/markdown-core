import MarkdownCoreC

/// A one-based line/column source coordinate.
public struct Position: Sendable, Hashable {
    /// The one-based line number.
    public let line: Int32
    /// The one-based column number.
    public let column: Int32

    /// Creates a coordinate from one-based line and column numbers.
    public init(line: Int32, column: Int32) {
        self.line = line
        self.column = column
    }
}

/// A node's absolute source extent: start and end coordinates, both
/// inclusive of the construct's own markers.
public struct Scope: Sendable, Hashable {
    /// The extent's first coordinate.
    public let start: Position
    /// The extent's last coordinate.
    public let end: Position

    /// Creates an extent from its boundary coordinates.
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
    /// The owning session's random salt; ids from different sessions never
    /// compare equal even when raw values collide.
    public let lineage: UInt64
    /// The id's value within its lineage: unique in the owning session and
    /// never reused.
    public let rawValue: UInt64

    /// Creates an identity from a session lineage and a raw id value.
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
    /// Two nodes are equal exactly when they share `id` and `revision`,
    /// which the engine guarantees implies identical content.
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.id == rhs.id && lhs.revision == rhs.revision
    }

    /// Hashes the identity/revision pair that also defines equality.
    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
        hasher.combine(revision)
    }
}

/// Whether a construct is embedded in inline content or stands alone
/// as its own block.
public enum PlacementMode: String, Sendable {
    case embedded
    case standalone
}
