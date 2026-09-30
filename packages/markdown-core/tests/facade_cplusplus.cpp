#include <markdown_core.h>

#include <type_traits>

static_assert(std::is_standard_layout<markdown_core_string>::value, "markdown_core_string must cross the C++ boundary");

int main() {
    markdown_core_document *document = nullptr;
    if (markdown_core_document_parse(nullptr, 0, &document) != MARKDOWN_CORE_OK || !document) {
        return 1;
    }
    const auto *root = markdown_core_document_root(document);
    const bool valid = markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT;
    markdown_core_document_free(document);
    return valid ? 0 : 1;
}
