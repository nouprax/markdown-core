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

public protocol Markup: Sendable {
    var scope: Scope { get }
    func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result
    func dump() -> String
}

public enum PlacementMode: String, Sendable {
    case embedded
    case standalone
}

extension Markup {
    static func scope(from node: OpaquePointer) -> Scope {
        Scope(from: markdown_core_node_scope(node))
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
