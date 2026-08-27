import MarkdownCoreC

/// One directive attribute. The sequence is sorted by name, so a pair is all
/// there is to say about one entry.
public struct DirectiveAttribute: Sendable, Hashable {
    /// The attribute's name, as written.
    public let name: String
    /// The attribute's value. A bare attribute has an empty one.
    public let value: String

    /// Creates an attribute. A bare attribute takes an empty value; the two
    /// are not the same as an absent attribute container.
    public init(name: String, value: String) {
        self.name = name
        self.value = value
    }
}

struct DirectiveValues {
    let name: String
    let attributes: [DirectiveAttribute]?

    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var nativeName = markdown_core_string()
        var hasAttributes = false
        var count = 0
        markdown_core_node_directive_properties(node, &nativeName, &hasAttributes, &count)
        name = nativeName.requiredString
        guard hasAttributes else {
            attributes = nil
            return
        }
        var pairs: [DirectiveAttribute] = []
        pairs.reserveCapacity(count)
        for index in 0..<count {
            var attributeName = markdown_core_string()
            var attributeValue = markdown_core_string()
            guard
                markdown_core_node_directive_attribute_at(node, index, &attributeName, &attributeValue)
            else { continue }
            pairs.append(
                DirectiveAttribute(
                    name: attributeName.requiredString,
                    value: attributeValue.requiredString
                )
            )
        }
        attributes = pairs
    }
}
