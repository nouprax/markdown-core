import MarkdownCoreC

public struct Heading: Markup {
    public let scope: Scope
    public let children: [any Markup]
    public let level: Int32

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Heading {
    init(from node: OpaquePointer) {
        var level: Int32 = 0
        markdown_core_node_heading_level(node, &level)
        self.init(scope: Self.scope(from: node), children: Self.children(from: node), level: level)
    }
}
