#include <cstdlib>
#include "directive.h"
#include <cstring>

#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "parser.h"
#include "markdown_core.h"
#include "cplusplus.h"
#include "harness.h"

static bool attach_extension(markdown_core_parser *parser, void *context) {
    const auto *extension = static_cast<const markdown_core_extension *>(context);
    return markdown_core_parser_attach_extension(parser, extension) != 0;
}

void test_cplusplus(test_batch_runner *runner) {
    static const char md[] = "paragraph\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *first = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(first), MARKDOWN_CORE_NODE_PARAGRAPH, "libmarkdown_core works with C++");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(first)), "paragraph",
           "parsed literals are readable from C++");
    markdown_core_node_free(doc);

    static const char directive_markdown[] = ":cpp{title=\"My Video\" id=ordinary muted=true}\n";
    const markdown_core_extension *extension = &MARKDOWN_CORE_EXTENSION_DIRECTIVE;
    markdown_core_node *document = markdown_core_parse_document_with_mem(
        directive_markdown, sizeof(directive_markdown) - 1, MARKDOWN_CORE_OPT_DEFAULT,
        markdown_core_get_default_mem_allocator(), attach_extension, const_cast<markdown_core_extension *>(extension));
    markdown_core_node *paragraph = markdown_core_node_first_child(document);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);
    {
        /* Reaches the attribute sequence from C++ -- what this case is for is
         * that the headers compile and link there, not the grammar. */
        markdown_core_string name{}, value{};
        INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 2, "universal records in C++");
        OK(runner, markdown_core_node_attribute_record_at(directive, 0, &name, &value), "record readable in C++");
        OK(runner, name.length == 5 && memcmp(name.data, "title", 5) == 0, "record order in C++");
        OK(runner, markdown_core_node_anchor(directive).has_value, "anchor readable in C++");
    }
    markdown_core_node_free(document);
}
