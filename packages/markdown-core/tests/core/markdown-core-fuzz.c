#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "parser.h"

const char *extension_names[] = {
    "autolink",
    "strikethrough",
    "table",
    NULL,
};

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

static bool attach_core_extensions(markdown_core_parser *parser, void *context) {
    return markdown_core_core_extensions_attach(parser, *(const unsigned *)context) != 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct __attribute__((packed)) {
        int options;
    } fuzz_config;

    if (size >= sizeof(fuzz_config)) {
        /* The beginning of `data` is treated as fuzzer configuration */
        memcpy(&fuzz_config, data, sizeof(fuzz_config));

        /* Remainder of input is the markdown */
        const char *markdown = (const char *)(data + sizeof(fuzz_config));
        const size_t markdown_size = size - sizeof(fuzz_config);
        /* A name selects a BIT; only the fixed table turns a set of bits into a
         * sequence. Attaching from the name list directly was a second attach
         * order, which is D15's shape. */
        unsigned extension_mask = 0;
        for (const char **it = extension_names; *it; ++it) {
            unsigned bit = markdown_core_core_extensions_bit(*it);
            if (!bit) {
                fprintf(stderr, "%s is not a valid parser extension\n", *it);
                abort();
            }
            extension_mask |= bit;
        }
        markdown_core_node *doc = markdown_core_parse_document_with_mem(markdown, markdown_size, fuzz_config.options,
                                                                        markdown_core_get_default_mem_allocator(),
                                                                        attach_core_extensions, &extension_mask);
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
    }
    return 0;
}
