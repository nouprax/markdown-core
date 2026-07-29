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

    /// Splits a directive's native child list into the contract's typed
    /// `label` prefix (nil when the directive declares no label — distinct
    /// from an explicit empty `[]`) and the remaining block content.
    func partition(_ children: [any Markup]) -> (label: [any Markup]?, content: [any Markup]) {
        guard let labelCount else { return (nil, children) }
        precondition(
            labelCount >= 0 && labelCount <= children.count,
            "native parser returned an invalid directive label count"
        )
        return (Array(children.prefix(labelCount)), Array(children.dropFirst(labelCount)))
    }
}
