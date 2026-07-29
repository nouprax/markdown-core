import MarkdownCoreC

struct DirectiveValues {
    let mode: PlacementMode
    let name: String
    let attributes: String?

    init(from node: OpaquePointer) {
        var nativeMode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var nativeName = markdown_core_string_view()
        var nativeAttributes = markdown_core_string_view()
        markdown_core_node_directive_properties(
            node,
            &nativeMode,
            &nativeName,
            &nativeAttributes
        )
        mode = PlacementMode(from: nativeMode)
        name = nativeName.requiredString
        attributes = nativeAttributes.optionalString
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
