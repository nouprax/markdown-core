import MarkdownCoreC

struct DirectiveValues {
    let mode: PlacementMode
    let name: String
    let attributes: String?
    let labelCount: Int?

    init(from node: OpaquePointer) {
        var nativeMode = MARKDOWN_CORE_PLACEMENT_EMBEDDED
        var nativeName = markdown_core_string_view()
        var nativeAttributes = markdown_core_string_view()
        var hasLabel = false
        var nativeLabelCount = 0
        markdown_core_node_directive_properties(
            node,
            &nativeMode,
            &nativeName,
            &nativeAttributes,
            &hasLabel,
            &nativeLabelCount
        )
        mode = PlacementMode(from: nativeMode)
        name = nativeName.requiredString
        attributes = nativeAttributes.optionalString
        labelCount = hasLabel ? nativeLabelCount : nil
    }
}
