import MarkdownCoreC

/// One editor source coordinate, copied using cmark's UTF-8 convention.
///
/// This is not a Swift string index. Native sentinel values are retained:
/// a zero-byte document ends at (0, 0), and a block can end at column 0.
/// No UTF-16, grapheme, half-open, or sentinel conversion is performed.
public struct Position: Sendable, Hashable {
    /// The native source line, normally 1-based; see the empty-document sentinel.
    public let line: Int32
    /// The native UTF-8 source column, including column-zero sentinels.
    public let column: Int32

    /// Creates a position from a line and a column. Neither is validated
    /// against any source; the parser is what produces meaningful pairs.
    public init(line: Int32, column: Int32) {
        self.line = line
        self.column = column
    }
}

/// The line-and-column range an element occupies.
///
/// A SCOPE IS A PAIR OF BOUNDARIES, NOT A BYTE RANGE. It tells an editor which
/// range of the source an element covers; it does not name a substring, and no
/// substring can be taken with it.
public struct Scope: Sendable, Hashable {
    /// The boundary the element begins at.
    public let start: Position
    /// The boundary the element ends at. It is not one past the last byte; it
    /// is where the element stops.
    public let end: Position

    /// Creates a scope from two boundaries. Neither is validated.
    public init(start: Position, end: Position) {
        self.start = start
        self.end = end
    }
}

/// One node of the parsed document.
///
/// Every kind is a value type and every kind is `Sendable`: the native parse is
/// released before ``Document/parse(_:)`` returns, so nothing here
/// borrows memory the C library owns and a tree can cross an isolation
/// boundary unchanged.
///
/// The set of conforming kinds is closed. ``MarkupVisitor`` and
/// ``MarkupWalkingVisitor`` name all of them, which makes both dispatch
/// protocols exhaustive at compile time.
public protocol Markup: Sendable {
    /// Where this element is, as a pair of boundaries. See ``Scope`` for what
    /// those boundaries are and are not.
    var scope: Scope { get }
    var anchor: String? { get }
    var attributes: Attributes { get }
    /// Dispatches to the visitor case for this element's kind.
    func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result
    /// The canonical debug dump of this element and everything under it.
    ///
    /// One grammar across C, Swift, Kotlin and ECMAScript, checked against the
    /// same goldens. It is a debugging and conformance surface, not a
    /// serialization format.
    func dump() -> String
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
    static func scope(from node: OpaquePointer) -> Scope {
        Scope(from: markdown_core_node_scope(node))
    }

}
