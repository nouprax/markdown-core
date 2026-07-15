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
