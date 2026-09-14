import MarkdownCoreC

/// A workspace link written as `[[...]]`.
public struct CrossLink: Markup {
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let dest: Destination
        let label: String?
    }

    let fields: Stored<Fields>

    /// The full authored extent, including the delimiters.
    public var scope: Scope { fields.read { $0.scope } }
    /// The declaration-side anchor, independent of the reference destination.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered attached classes and records.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The raw workspace path and optional destination anchor.
    public var dest: Destination { fields.read { $0.dest } }
    /// The raw authored label; nil when no separator was written.
    public var label: String? { fields.read { $0.label } }
}

extension CrossLink.Fields {
    init(from node: OpaquePointer) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            dest: Destination(from: node),
            label: markdown_core_node_cross_label(node).string
        )
    }
}
