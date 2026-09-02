import MarkdownCoreC

/// One boundary in the source: a line and a column, both counted from 1.
///
/// A position is not an index. `column` counts BOUNDARIES between bytes, so a
/// line of L bytes has boundaries 1 through L + 1, and column 0 is the
/// boundary before a line begins — which is where a block closed by a blank
/// line ends.
public struct Position: Sendable, Hashable {
    /// The 1-based source line.
    public let line: Int32
    /// The 1-based boundary within the line, counted in BYTES, not characters.
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
/// released before ``Document/parse(_:options:)`` returns, so nothing here
/// borrows memory the C library owns and a tree can cross an isolation
/// boundary unchanged.
///
/// The set of conforming kinds is closed. ``MarkupVisitor`` names all of them,
/// which is what makes a visitor exhaustive at compile time.
public protocol Markup: Sendable {
    /// Where this element is, as a pair of boundaries. See ``Scope`` for what
    /// those boundaries are and are not.
    var scope: Scope { get }
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
public enum PlacementMode: String, Sendable {
    /// Written inside a line, among other inline content.
    case embedded
    /// Written on its own, as a block.
    case standalone
}

extension Markup {
    static func scope(from node: OpaquePointer) -> Scope {
        Scope(from: markdown_core_node_scope(node))
    }

    /// Narrows an already-copied content relation to its semantic element kind.
    ///
    /// A `List` owns `ListItem`s and a `TableRow` owns `TableCell`s; the C tree
    /// cannot say so and the typed model can, so the narrowing happens once,
    /// here, instead of at every use site.
    static func typedChildren<T: Markup>(_ children: [any Markup]) -> [T] {
        children.map { child in
            guard let typed = child as? T else {
                preconditionFailure("\(type(of: child)) is not a \(T.self)")
            }
            return typed
        }
    }
}
