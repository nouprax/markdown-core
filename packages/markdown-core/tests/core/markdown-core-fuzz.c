#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "feature-registry.h"
#include "parser.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

static bool attach_core_extensions(markdown_core_parser *parser, void *context) {
    return markdown_core_core_extensions_attach(parser, *(const unsigned *)context) != 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The whole input is Markdown, parsed as the one dialect: every
     * registered feature, resolved by the registry into the engine's option
     * word and extension mask. The dialect has no switches, so there is no
     * configuration prefix to fuzz. */
    int options = 0;
    unsigned extension_mask = 0;
    markdown_core_node *doc;

    markdown_core_features_resolve(markdown_core_features_all(), &options, &extension_mask);
    doc = markdown_core_parse_document_with_mem((const char *)data, size, options,
                                                markdown_core_get_default_mem_allocator(), attach_core_extensions,
                                                &extension_mask);
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
