import MarkdownCoreC

/// One attribute assignment. Names and values are case sensitive.
public struct Record: Sendable, Hashable {
    /// The case-sensitive assignment name.
    public let name: String
    /// The value after attribute escape and entity decoding.
    public let value: String
    /// Creates one ordered assignment value.
    public init(name: String, value: String) {
        self.name = name
        self.value = value
    }
}

/// Classes and records retain source order and duplicates independently.
public struct Attributes: Sendable, Hashable {
    /// Class words in source order, including duplicates.
    public let classes: [String]
    /// Assignments other than `id` and `class`, in source order.
    public let records: [Record]
    /// The shared value for absent and explicitly empty containers.
    public static let empty = Attributes(classes: [], records: [])
    /// Creates attributes without sorting or deduplicating either sequence.
    public init(classes: [String], records: [Record]) {
        self.classes = classes
        self.records = records
    }
}

extension Attributes {
    init(from node: OpaquePointer) {
        self.init(
            classes: (0..<markdown_core_node_attribute_class_count(node)).map { index in
                var value = markdown_core_string()
                precondition(markdown_core_node_attribute_class_at(node, index, &value))
                return value.requiredString
            },
            records: (0..<markdown_core_node_attribute_record_count(node)).map { index in
                var name = markdown_core_string()
                var value = markdown_core_string()
                precondition(markdown_core_node_attribute_record_at(node, index, &name, &value))
                return Record(name: name.requiredString, value: value.requiredString)
            }
        )
    }
}
