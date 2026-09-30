/// One editor source coordinate, computed on request by a scope query.
///
/// This is not a Swift string index. The line counts from 1 and the column
/// from 1, in the document's ``TextUnit``. An end position names the columns
/// from its line's start, so a range that ends right after a line terminator
/// ends at column 0 of the next line.
public struct Position: Sendable, Hashable {
    /// The source line, counted from 1.
    public let line: Int32
    /// The source column in the document's unit, counted from 1; an end may
    /// be column 0.
    public let column: Int32

    /// Creates a position from a line and a column. Neither is validated
    /// against any source; a scope query is what produces meaningful pairs.
    public init(line: Int32, column: Int32) {
        self.line = line
        self.column = column
    }
}

/// The line-and-column range an element occupies.
///
/// A SCOPE IS A PAIR OF BOUNDARIES, NOT A BYTE RANGE. It tells an editor which
/// range of the source an element covers; it does not name a substring, and no
/// substring can be taken with it. A node stores no scope: ``Document`` computes
/// one from the node's ``Extent`` and the source when it is asked.
public struct Scope: Sendable, Hashable {
    /// The position of the element's first byte.
    public let start: Position
    /// The line holding the byte just past the element and the columns from
    /// that line's start to it. A zero-byte document is `1:1..1:0`.
    public let end: Position

    /// Creates a scope from two boundaries. Neither is validated.
    public init(start: Position, end: Position) {
        self.start = start
        self.end = end
    }
}

/// How a document counts the columns of its scopes: UTF-8 bytes or UTF-16
/// code units.
///
/// The unit is how positions are counted, not how the text is stored. Editor
/// surfaces on every platform count UTF-16 code units, so it is the default.
public enum TextUnit: Sendable, Hashable {
    /// UTF-8 bytes.
    case utf8
    /// UTF-16 code units; a scalar outside the Basic Multilingual Plane is two.
    case utf16
}

/// A node's identifier: unique within its document, and numbered from 1 in
/// canonical walk order by a parse, so two parses of the same text agree.
///
/// Identifiers from different documents are not comparable. Every value is
/// below 2^53.
public struct MarkupID: Hashable, Sendable {
    /// The identifier's integer value.
    public let value: UInt64

    init(_ value: UInt64) {
        self.value = value
    }
}

/// Where a node is, in bytes of the UTF-8 source, relative to its neighbours.
///
/// `lead` is the signed distance from the end of the previous node in the same
/// relation (or from the owner's start, for the first node of a relation) to
/// this node's start, and `span` the length of its source range. Neither
/// changes when text before the node moves. Scopes are computed from extents
/// and the source on request; see ``Document/scope(of:in:)``.
public struct Extent: Sendable, Hashable {
    /// The signed byte distance from the node's anchor to its start.
    public let lead: Int32
    /// The node's length in bytes.
    public let span: UInt32

    /// Creates an extent. Neither number is validated.
    public init(lead: Int32, span: UInt32) {
        self.lead = lead
        self.span = span
    }
}

/// One node of the parsed document.
///
/// Every kind is an immutable value and every kind is `Sendable`: the native
/// parse is released before ``Document/parse(_:unit:)`` returns, so nothing
/// here borrows memory the C library owns and a tree can cross an isolation
/// boundary unchanged.
///
/// Equality is deep value equality including ``id``: two nodes are equal when
/// they have the same kind, id, fields, extent and pairwise equal children in
/// every relation. Hashing reads only the id. For two existentials, use
/// ``isEqual(_:)``.
///
/// The set of conforming kinds is closed. ``MarkupVisitor`` names all of them,
/// making traversal callbacks exhaustive at compile time.
public protocol Markup: Hashable, Identifiable, Sendable, CustomStringConvertible where ID == MarkupID {
    /// The node's identifier within its document.
    var id: MarkupID { get }
    /// Where the node is, relative to its neighbours. See ``Extent``.
    var extent: Extent { get }
    var anchor: String? { get }
    var attributes: Attributes { get }
}

/// Whether the source wrote a formula inside a line or on its own.
///
/// It survives only on ``Formula``, which is the one kind where it is a fact
/// about the source rather than about the kind: an inline ``Directive`` is
/// always embedded and a ``DirectiveBlock`` always standalone, so carrying it
/// there made four surfaces keep a constant in step.
public enum Placement: String, Sendable {
    /// Written inside a line, among other inline content.
    case embedded
    /// Written on its own, as a block.
    case standalone
}

extension Markup {
    /// Deep value equality with a node of any kind, as `==` is for two nodes
    /// of one kind.
    public func isEqual(_ other: any Markup) -> Bool {
        MarkupRecord.of(self) == MarkupRecord.of(other)
    }

    /// The node's kind and ``id``, as `Paragraph(id=3)`. It reads no
    /// descendant; ``Document/dump(in:)`` draws a whole tree.
    public var description: String {
        "\(type(of: self))(id=\(id.value))"
    }
}
