#include <cstdlib>
#include "directive.h"
#include <cstring>

#include "node_type.h"
#include "markdown-core-elements.h"
#include "parser.h"
#include "markdown_core.h"
#include "cplusplus.h"
#include "harness.h"

void test_cplusplus(test_batch_runner *runner) {
    static const char md[] = "paragraph\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1);
    markdown_core_node *first = doc->first_child;
    INT_EQ(runner, first->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "libmarkdown_core works with C++");
    markdown_core_string literal = markdown_core_node_literal(first->first_child);
    OK(runner, literal.length == 9 && memcmp(literal.data, "paragraph", 9) == 0,
       "parsed literals are readable from C++");
    markdown_core_node_free(doc);

    static const char directive_markdown[] = ":cpp{title=\"My Video\" id=ordinary muted=true}\n";
    markdown_core_node *document =
        markdown_core_parse_document_with_setup(directive_markdown, sizeof(directive_markdown) - 1, NULL, NULL);
    markdown_core_node *paragraph = document->first_child;
    markdown_core_node *directive = paragraph->first_child;
    {
        /* Reaches the attribute sequence from C++ -- what this case is for is
         * that the headers compile and link there, not the grammar. */
        markdown_core_string name{}, value{};
        INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 2, "universal records in C++");
        markdown_core_node_attribute_record_at(directive, 0, &name, &value);
        OK(runner, name.length == 5 && memcmp(name.data, "title", 5) == 0, "record order in C++");
        OK(runner, markdown_core_node_anchor(directive).has_value, "anchor readable in C++");
    }
    markdown_core_node_free(document);
}
