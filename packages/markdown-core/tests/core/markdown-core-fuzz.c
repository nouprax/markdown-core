#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"

const char *extension_names[] = {
    "autolink",
    "strikethrough",
    "table",
    NULL,
};

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

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
        markdown_core_parser *parser = markdown_core_parser_new(fuzz_config.options);

        for (const char **it = extension_names; *it; ++it) {
            const char *extension_name = *it;
            markdown_core_extension *extension = markdown_core_find_extension(extension_name);
            if (!extension) {
                fprintf(stderr, "%s is not a valid syntax extension\n", extension_name);
                abort();
            }
            markdown_core_parser_attach_extension(parser, extension);
        }

        markdown_core_parser_feed(parser, markdown, markdown_size);
        markdown_core_node *doc = markdown_core_parser_finish(parser);

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
        markdown_core_parser_free(parser);
    }
    return 0;
}
