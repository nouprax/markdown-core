import MarkdownCoreC

struct DirectiveValues {
    let mode: PlacementMode
    let name: String
    let attributes: [String: String]?

    init(from node: OpaquePointer) {
        var nativeMode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var nativeName = markdown_core_string()
        var present = false
        markdown_core_node_directive_properties(
            node,
            &nativeMode,
            &nativeName,
            &present
        )
        mode = PlacementMode(from: nativeMode)
        name = nativeName.string
        guard present else {
            attributes = nil
            return
        }
        // The grammar has no duplicate name to represent — a later `k=`
        // replaces the earlier one, `.a .b` accumulates into one `class` —
        // so the pairs the engine holds ARE the map.
        var count = 0
        markdown_core_node_directive_attribute_count(node, &count)
        var pairs: [String: String] = Dictionary(minimumCapacity: count)
        for index in 0..<count {
            var key = markdown_core_string()
            var value = markdown_core_string()
            guard markdown_core_node_directive_attribute_at(node, index, &key, &value) else { break }
            pairs[key.string] = value.string
        }
        attributes = pairs
    }

    /// Binds the directive's optional first `DirectiveLabel` child to its
    /// typed property and returns the remaining block content.
    func partition(
        _ children: [any Markup]
    ) -> (label: DirectiveLabel?, content: [any Markup]) {
        let label = children.first as? DirectiveLabel
        precondition(
            children.dropFirst(label == nil ? 0 : 1).allSatisfy { !($0 is DirectiveLabel) },
            "directive-label node must be the directive's unique first child"
        )
        return (label, Array(children.dropFirst(label == nil ? 0 : 1)))
    }
}
