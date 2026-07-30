import MarkdownCoreC

/// A cross-link written as `[[reference]]`.
public struct CrossLink: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The source-faithful reference between the delimiters.
    public let reference: String

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension CrossLink {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var reference = markdown_core_string_view()
        precondition(markdown_core_node_cross_link_reference(node, &reference))
        self.init(id: id, revision: revision, reference: reference.requiredString)
    }
}
