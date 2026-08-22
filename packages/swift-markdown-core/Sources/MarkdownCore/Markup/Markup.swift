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

    /// A directive's label children, or `nil` when the source wrote no label.
    ///
    /// `label` and `content` are two runs of one child list: the C facade names
    /// where each begins and `label_count` says how long the first one is.
    /// Until Step 15A the Swift model kept the COUNT and threw the nodes into a
    /// flat `children`, so `label` was an `Int?` in a contract that says
    /// `[Markup]?`.
    static func directiveLabel(from node: OpaquePointer, count: Int?) -> [any Markup]? {
        guard let count else { return nil }
        var result: [any Markup] = []
        result.reserveCapacity(count)
        var child = markdown_core_node_directive_first_label_child(node)
        while let current = child, result.count < count {
            result.append(markup(from: current))
            child = markdown_core_node_get_next_sibling(current)
        }
        return result
    }

    /// A directive's content children: everything after the label.
    static func directiveContent(from node: OpaquePointer) -> [any Markup] {
        var result: [any Markup] = []
        var child = markdown_core_node_directive_first_content_child(node)
        while let current = child {
            result.append(markup(from: current))
            child = markdown_core_node_get_next_sibling(current)
        }
        return result
    }
}
