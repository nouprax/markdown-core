import MarkdownCoreC

/// A workspace transclusion written as `![[...]]`.
public struct CrossEmbedded: Markup {
    let record: CrossEmbeddedRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The full authored extent, including the delimiters.
    public var extent: Extent { record.extent }
    /// The source it read, and where its content was read from. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The declaration-side anchor, independent of the reference destination.
    public var anchor: String? { record.anchor }
    /// Ordered attached classes and records.
    public var attributes: Attributes { record.attributes }
    /// The raw workspace path and optional destination anchor.
    public var dest: Destination { record.dest }
    /// Raw prefix after a valid dimension suffix; nil when no separator was written.
    public var label: String? { record.label }
    /// Authored size, absent when no complete valid suffix was recognized.
    public var dimensions: Dimensions? { record.dimensions }
}

final class CrossEmbeddedRecord: MarkupRecord, @unchecked Sendable {
    let dest: Destination
    let label: String?
    let dimensions: Dimensions?

    init(_ fields: InheritedFields, dest: Destination, label: String?, dimensions: Dimensions?) {
        self.dest = dest
        self.label = label
        self.dimensions = dimensions
        super.init(fields, children: [])
    }

    override var markup: any Markup { CrossEmbedded(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: CrossEmbeddedRecord.self)
        return dest == other.dest && label == other.label && dimensions == other.dimensions
    }
}

extension CrossEmbeddedRecord {
    convenience init(from node: OpaquePointer) {
        self.init(
            InheritedFields(from: node),
            dest: Destination(from: node),
            label: answer(markdown_core_optional_string()) { markdown_core_node_cross_label(node, $0) }.string,
            dimensions: answer(nil) { markdown_core_node_dimensions(node, $0) }.map { Dimensions($0.pointee) }
        )
    }
}

extension CrossEmbedded: RecordBacked {
    var base: MarkupRecord { record }
}
