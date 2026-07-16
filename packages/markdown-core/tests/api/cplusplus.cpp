#include <cstdlib>
#include <cstring>

#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "cplusplus.h"
#include "harness.h"

void test_cplusplus(test_batch_runner *runner) {
    static const char md[] = "paragraph\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1, MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *first = markdown_core_node_first_child(doc);
    INT_EQ(runner, markdown_core_node_get_type(first), MARKDOWN_CORE_NODE_PARAGRAPH, "libmarkdown_core works with C++");
    STR_EQ(runner, markdown_core_node_get_literal(markdown_core_node_first_child(first)), "paragraph",
           "parsed literals are readable from C++");
    markdown_core_node_free(doc);

    static const char directive_markdown[] = ":cpp{id=ordinary title=\"My Video\" muted=true}\n";
    static const char directive_attributes[] = "{\"id\":\"ordinary\",\"title\":\"My Video\",\"muted\":\"true\"}";
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DIRECTIVE);
    markdown_core_extension *extension = markdown_core_find_extension("directive");
    markdown_core_parser_attach_extension(parser, extension);
    markdown_core_parser_feed(parser, directive_markdown, sizeof(directive_markdown) - 1);
    markdown_core_node *document = markdown_core_parser_finish(parser);
    markdown_core_node *paragraph = markdown_core_node_first_child(document);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive), directive_attributes,
           "directive attributes normalize to string-map JSON in C++");
    INT_EQ(runner, markdown_core_extensions_set_directive_attributes(directive, "{ \"replacement\" : \"exact\" }"), 1,
           "directive string-map JSON setter works with C++");
    STR_EQ(runner, markdown_core_extensions_get_directive_attributes(directive), "{\"replacement\":\"exact\"}",
           "directive JSON setter returns normalized string-map JSON in C++");
    markdown_core_node_free(document);
    markdown_core_parser_free(parser);
}
