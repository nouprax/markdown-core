import MarkdownCoreC

struct DirectiveValues {
    let name: String
    init(from node: OpaquePointer) {
        var nativeName = markdown_core_string()
        markdown_core_node_directive_properties(node, &nativeName)
        name = nativeName.requiredString
    }
}
