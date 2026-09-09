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
        self.init(from: markdown_core_node_primary_attributes(node))
    }

    init(from value: OpaquePointer?) {
        self.init(
            classes: (0..<markdown_core_attribute_value_class_count(value)).map { index in
                var string = markdown_core_string()
                precondition(markdown_core_attribute_value_class_at(value, index, &string))
                return string.requiredString
            },
            records: (0..<markdown_core_attribute_value_record_count(value)).map { index in
                var name = markdown_core_string()
                var string = markdown_core_string()
                precondition(markdown_core_attribute_value_record_at(value, index, &name, &string))
                return Record(name: name.requiredString, value: string.requiredString)
            }
        )
    }
}

extension Attributes {
    /// Native arrays keep Swift value semantics and copy-on-write storage. Only
    /// a nonempty occurrence sequence needs a new array when it is appended.
    func inheriting(_ inherited: Attributes) -> Attributes {
        Attributes(
            classes: classes.isEmpty ? inherited.classes : inherited.classes + classes,
            records: records.isEmpty ? inherited.records : inherited.records + records
        )
    }
}
