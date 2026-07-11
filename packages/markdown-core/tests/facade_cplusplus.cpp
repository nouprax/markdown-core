#include <markdown_core.h>

#include <type_traits>

static_assert(std::is_standard_layout<markdown_core_parse_options>::value, "parse options must cross the C++ boundary");
static_assert(std::is_standard_layout<markdown_core_string_view>::value, "string views must cross the C++ boundary");

int main() {
    markdown_core_parse_options options{};
    markdown_core_parse_options_init(&options);
    markdown_core_error *error = nullptr;
    const auto *document = markdown_core_document_parse(nullptr, 0, &options, &error);
    if (!document || error)
        return 1;
    const auto *root = markdown_core_document_root(document);
    const bool valid = markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT;
    markdown_core_document_free(const_cast<markdown_core_document *>(document));
    return valid ? 0 : 1;
}
