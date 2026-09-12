#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-elements.h"
#include "parser.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The whole input is Markdown, parsed as the one dialect: the engine always
     * attaches every element. The dialect has no
     * switches, so there is no configuration prefix to fuzz. */
    markdown_core_node *doc = markdown_core_parse_document_with_mem(
        (const char *)data, size, markdown_core_get_default_mem_allocator(), NULL, NULL);
    if (!doc) {
        return 0;
    }

    /* Exercise every node and accessor instead of the retired renderers:
     * parse, traverse, and free. */
    markdown_core_iter *iter = markdown_core_iter_new(doc);
    markdown_core_event_type ev_type;
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        (void)markdown_core_node_get_type(node);
        (void)markdown_core_node_get_literal(node);
        (void)markdown_core_node_get_start_line(node);
        (void)markdown_core_node_get_end_column(node);
    }
    markdown_core_iter_free(iter);

    markdown_core_node_free(doc);
    return 0;
}
