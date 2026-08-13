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

/// Series-scoped node identity, stable across edits while the node remains the
/// same kind of thing at the same place.
///
/// ``rawValue`` is unique within the owning series and never reused;
/// ``series`` is that series' random salt, so nodes from different series
/// (including separate one-shot parses) never share an identity.
///
/// A SERIES is one document and every document its edits produce. Raw values
/// restart at 1 for each new series, so the salt is the only thing keeping
/// two unrelated documents' identities apart.
public struct MarkupID: Sendable, Hashable {
    /// The owning series' random salt; ids from different series never
    /// compare equal even when raw values collide.
    public let series: UInt64
    /// The id's value within its series: unique there and never reused.
    public let rawValue: UInt64

    /// Creates an identity from a series salt and a raw id value.
    public init(series: UInt64, rawValue: UInt64) {
        self.series = series
        self.rawValue = rawValue
    }
}

/// Identity, revision, and extent: what every kind carries whatever it is.
///
/// The contract calls this the node's `MarkupTrack`.
struct MarkupTrack {
    let id: MarkupID
    let revision: UInt64
    let scope: Scope
}

/// A node of the canonical Markdown value tree.
///
/// Nodes are immutable values. Equality and hashing are O(1) and
/// allocation-free: two nodes are equal exactly when they have the same
/// ``id`` and the same ``revision``, which the engine guarantees implies
/// identical AST content (fields and descendants).
///
/// ``scope`` is on every node and is deliberately NOT part of that: absolute
/// source position is not content, so two nodes differing only in where they
/// sit are equal. That is what lets an edit above a node leave every reactive
/// comparison below it untouched.
public protocol Markup: Sendable, Identifiable, Hashable where ID == MarkupID {
    /// The node's series-scoped identity; see ``MarkupID``.
    var id: MarkupID { get }

    /// The document revision at which this node's own fields, child list, or
    /// any descendant last changed.
    ///
    /// A pure positional shift caused by an edit elsewhere never changes a
    /// node's revision.
    var revision: UInt64 { get }

    /// The node's absolute source extent, O(1) and stored on the value.
    var scope: Scope { get }

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result
}

extension Markup {
    /// Two nodes are equal exactly when they share ``id`` and ``revision``,
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
