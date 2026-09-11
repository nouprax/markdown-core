import MarkdownCoreC

/// An ordered, nonempty list of term/body associations.
public struct DefinitionList: Markup {
    /// The authored source range, including the term and all bodies.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The nonempty ordered collection of term/body associations.
    public let definitions: [Definition]

    /// Dispatches to the visitor's `DefinitionList` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DefinitionList {
    init(from node: OpaquePointer, children: [any Markup]) {
        precondition(!children.isEmpty, "Empty definition list")
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            definitions: children.map { child in
                guard let definition = child as? Definition else {
                    preconditionFailure("Invalid definition list child")
                }
                return definition
            }
        )
    }
}
