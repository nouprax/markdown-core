#include <cstdlib>
#include "directive.h"
#include <cstring>

#include "node_type.h"
#include "markdown-core-elements.h"
#include "parser.h"
#include "markdown_core.h"
#include "cplusplus.h"
#include "harness.h"

/* The first node a cursor reads below `node`, or NULL when it has none: how
 * C++ reads a published tree, through the public cursor. */
static const markdown_core_node *first_read(const markdown_core_node *node) {
    markdown_core_cursor *cursor;
    const markdown_core_node *found = NULL;
    bool moved = false;
    if (markdown_core_cursor_open(node, &cursor) != MARKDOWN_CORE_OK) {
        return NULL;
    }
    if (markdown_core_cursor_child(cursor, &moved) == MARKDOWN_CORE_OK && moved) {
        found = markdown_core_cursor_node(cursor);
    }
    markdown_core_cursor_free(cursor);
    return found;
}

void test_cplusplus(test_batch_runner *runner) {
    static const char md[] = "paragraph\n";
    markdown_core_node *doc = markdown_core_parse_document(md, sizeof(md) - 1);
    const markdown_core_node *first = first_read(doc);
    INT_EQ(runner, first->kind, MARKDOWN_CORE_NODE_PARAGRAPH, "libmarkdown_core works with C++");
    markdown_core_string literal{};
    OK(runner,
       markdown_core_node_literal(first_read(first), &literal) == MARKDOWN_CORE_OK && literal.length == 9 &&
           memcmp(literal.data, "paragraph", 9) == 0,
       "parsed literals are readable from C++");
    markdown_core_node_free(doc);

    static const char directive_markdown[] = ":cpp{title=\"My Video\" id=ordinary muted=true}\n";
    markdown_core_node *document =
        markdown_core_parse_document_with_setup(directive_markdown, sizeof(directive_markdown) - 1, NULL, NULL);
    const markdown_core_node *paragraph = first_read(document);
    const markdown_core_node *directive = first_read(paragraph);
    {
        /* Reaches the attribute sequence from C++ -- what this case is for is
         * that the headers compile and link there, not the grammar. */
        markdown_core_string name{}, value{};
        INT_EQ(runner, (int)markdown_core_node_attribute_record_count(directive), 2, "universal records in C++");
        OK(runner,
           markdown_core_node_attribute_record_at(directive, 0, &name, &value) == MARKDOWN_CORE_OK &&
               name.length == 5 && memcmp(name.data, "title", 5) == 0,
           "record order in C++");
        OK(runner, markdown_core_node_anchor(directive).has_value, "anchor readable in C++");
    }
    markdown_core_node_free(document);
}
