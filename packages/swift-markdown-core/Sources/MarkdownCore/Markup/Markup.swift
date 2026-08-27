import MarkdownCoreC

/// One boundary in the source: a line and a column, both counted from 1.
///
/// A position is not an index. `column` counts BOUNDARIES between bytes, so a
/// line of L bytes has boundaries 1 through L + 1, and column 0 is the
/// boundary before a line begins — which is where a block closed by a blank
/// line ends.
public struct Position: Sendable, Hashable {
    /// The 1-based line, counted in ``Concrete/source``.
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
/// substring can be taken with it. Slicing the source between two positions is
/// a misuse — the coordinates are counted against ``Concrete/source``, which
/// is the normalized text and not the string that was fed to the ``Document``.
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

/// A node's identity: the name a consumer tracks an element by across a
/// stream's feeds — the render key (docs/STREAMING.md §4 D4).
///
/// ``block`` is the owning block's document-unique mint — the block is the
/// minimal update unit, so it alone names the region an incremental consumer
/// re-renders — and ``ordinal`` is the node's pre-order ordinal among that
/// block's inline descendants, 0 for the block itself. The pair is unique
/// within one document and never reused within a parse; it is not stable
/// across documents. The halves are opaque values: compare them, key
/// dictionaries by them, and derive nothing else from them.
public struct Identity: Sendable, Hashable {
    /// The owning block's document-unique mint; a block's own.
    public let block: UInt32
    /// The pre-order ordinal within the owning block; 0 for the block itself.
    public let ordinal: UInt32

    /// Creates an identity from its two halves. Neither is validated; the
    /// parser is what produces meaningful pairs.
    public init(block: UInt32, ordinal: UInt32) {
        self.block = block
        self.ordinal = ordinal
    }
}

extension Identity {
    init(from value: markdown_core_identity) {
        self.init(block: value.block, ordinal: value.ordinal)
    }
}

/// One node of the parsed document.
///
/// Every kind is a value type and every kind is `Sendable`: the native parse
/// is released before a ``Read`` is returned, so nothing here borrows memory
/// the C library owns and a tree can cross an isolation boundary unchanged.
///
/// The set of conforming kinds is closed. ``Visitor`` names all of them,
/// which is what makes a visitor exhaustive at compile time.
public protocol Markup: Sendable {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    var id: Identity { get }
    /// Where this element is, as a pair of boundaries. See ``Scope`` for what
    /// those boundaries are and are not.
    var scope: Scope { get }
    /// Dispatches to the visitor case for this element's kind.
    func accept<V: Visitor>(_ visitor: inout V) -> V.Result
    /// The canonical diagnostic dump of this element and everything under it.
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

    /// The node's whole identity, answered by the C side from the node alone
    /// — the engine stamps an inline's owner in the same pass that assigns
    /// its ordinal, so no walk here composes anything.
    static func identity(from node: OpaquePointer) -> Identity {
        Identity(from: markdown_core_node_identity(node))
    }

    /// Every child, in source order, as the C tree holds them.
    static func children(from node: OpaquePointer) -> [any Markup] {
        var result: [any Markup] = []
        result.reserveCapacity(markdown_core_node_child_count(node))
        var child = markdown_core_node_get_first_child(node)
        while let current = child {
            result.append(markup(from: current))
            child = markdown_core_node_get_next_sibling(current)
        }
        return result
    }
}

/// The definition edge a reference carries: the identity of the definition it
/// resolved to — the first definition of its label in document order. The
/// target is a block, so its ordinal is 0 by construction.
func referenceDefinition(from node: OpaquePointer) -> Identity {
    var definition = markdown_core_identity()
    markdown_core_node_reference_definition(node, &definition)
    return Identity(from: definition)
}

extension Markup {
    /// Every child, required to be one kind.
    ///
    /// A `List` owns `ListItem`s and a `TableRow` owns `TableCell`s; the C tree
    /// cannot say so and the typed model can, so the narrowing happens once,
    /// here, instead of at every use site.
    static func typedChildren<T: Markup>(from node: OpaquePointer) -> [T] {
        children(from: node).map { child in
            guard let typed = child as? T else {
                preconditionFailure("\(type(of: child)) is not a \(T.self)")
            }
            return typed
        }
    }

    /// A directive's label, or `nil` when the source wrote none.
    ///
    /// The label is the first child when it is there at all, so this is a
    /// look, not a search. Until Step 7 the C facade spliced the label node
    /// out of the child list and named its count on the parent, and this
    /// walked a run of children with no container; the node is visible now.
    static func directiveLabel(from node: OpaquePointer) -> DirectiveLabel? {
        guard let first = markdown_core_node_get_first_child(node),
            markdown_core_node_get_kind(first) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL
        else { return nil }
        return DirectiveLabel(from: first)
    }

    /// A directive block's content: every child after the label.
    static func directiveContent(from node: OpaquePointer) -> [any Markup] {
        var result: [any Markup] = []
        var child = markdown_core_node_get_first_child(node)
        if let first = child, markdown_core_node_get_kind(first) == MARKDOWN_CORE_KIND_DIRECTIVE_LABEL {
            child = markdown_core_node_get_next_sibling(first)
        }
        while let current = child {
            result.append(markup(from: current))
            child = markdown_core_node_get_next_sibling(current)
        }
        return result
    }
}
