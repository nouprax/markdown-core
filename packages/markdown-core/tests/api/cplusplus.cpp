#include <cstdlib>
#include "directive.h"
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
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DIRECTIVE);
    const markdown_core_syntax_extension *extension = &MARKDOWN_CORE_EXTENSION_DIRECTIVE;
    markdown_core_parser_attach_syntax_extension(parser, extension);
    markdown_core_parser_feed(parser, directive_markdown, sizeof(directive_markdown) - 1);
    markdown_core_node *document = markdown_core_parser_finish(parser);
    markdown_core_node *paragraph = markdown_core_node_first_child(document);
    markdown_core_node *directive = markdown_core_node_first_child(paragraph);
    {
        /* Reaches the attribute sequence from C++ -- what this case is for is
         * that the headers compile and link there, not the grammar. */
        const char *name = nullptr;
        const char *value = nullptr;
        size_t name_length = 0;
        size_t value_length = 0;
        INT_EQ(runner, markdown_core_extensions_directive_has_attributes(directive), 1,
               "directive reports an attribute container in C++");
        INT_EQ(runner, (int)markdown_core_extensions_directive_attribute_count(directive), 3,
               "directive attribute count in C++");
        INT_EQ(
            runner,
            markdown_core_extensions_directive_attribute_at(directive, 0, &name, &name_length, &value, &value_length),
            1, "directive attribute read in C++");
        OK(runner, name_length == 2 && memcmp(name, "id", 2) == 0, "directive attributes are sorted by name in C++");
    }
    markdown_core_node_free(document);
    markdown_core_parser_free(parser);
}
