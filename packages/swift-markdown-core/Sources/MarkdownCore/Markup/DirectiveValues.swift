import MarkdownCoreC

struct DirectiveValues {
    let name: String
    let attributes: String?
    let labelCount: Int?

    init(from node: OpaquePointer) {
        var nativeName = markdown_core_string_view()
        var nativeAttributes = markdown_core_string_view()
        var hasLabel = false
        var nativeLabelCount = 0
        markdown_core_node_directive_properties(
            node,
            &nativeName,
            &nativeAttributes,
            &hasLabel,
            &nativeLabelCount
        )
        name = nativeName.requiredString
        attributes = nativeAttributes.optionalString
        labelCount = hasLabel ? nativeLabelCount : nil
    }
}
