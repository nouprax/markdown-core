#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "feature-registry.h"
#include "parser.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

static bool attach_core_extensions(markdown_core_parser *parser, void *context) {
    return markdown_core_core_extensions_attach(parser, *(const unsigned *)context) != 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct __attribute__((packed)) {
        uint8_t splitpoint;
        uint8_t repeatlen;
    } fuzz_config;

    if (size >= sizeof(fuzz_config)) {
        /* The beginning of `data` is treated as fuzzer configuration */
        memcpy(&fuzz_config, data, sizeof(fuzz_config));

        /* Remainder of input is the markdown */
        const char *markdown0 = (const char *)(data + sizeof(fuzz_config));
        const size_t markdown_size0 = size - sizeof(fuzz_config);
        char markdown[0x80000];
        if (markdown_size0 <= sizeof(markdown)) {
            size_t markdown_size = 0;
            if (fuzz_config.splitpoint <= markdown_size0 && 0 < fuzz_config.repeatlen &&
                fuzz_config.repeatlen <= markdown_size0 - fuzz_config.splitpoint) {
                const size_t size_after_splitpoint = markdown_size0 - fuzz_config.splitpoint - fuzz_config.repeatlen;
                memcpy(&markdown[markdown_size], &markdown0[0], fuzz_config.splitpoint);
                markdown_size += fuzz_config.splitpoint;

                while (markdown_size + fuzz_config.repeatlen + size_after_splitpoint <= sizeof(markdown)) {
                    memcpy(&markdown[markdown_size], &markdown0[fuzz_config.splitpoint], fuzz_config.repeatlen);
                    markdown_size += fuzz_config.repeatlen;
                }
                memcpy(&markdown[markdown_size], &markdown0[fuzz_config.splitpoint + fuzz_config.repeatlen],
                       size_after_splitpoint);
                markdown_size += size_after_splitpoint;
            } else {
                markdown_size = markdown_size0;
                memcpy(markdown, markdown0, markdown_size);
            }

            /* The one dialect: the registry resolves every feature into the
             * engine's option word and extension mask, and the fixed table
             * turns the mask into the one attach order. */
            int options = 0;
            unsigned extension_mask = 0;
            markdown_core_features_resolve(markdown_core_features_all(), &options, &extension_mask);
            markdown_core_node *doc = markdown_core_parse_document_with_mem(markdown, markdown_size, options,
                                                                            markdown_core_get_default_mem_allocator(),
                                                                            attach_core_extensions, &extension_mask);
            if (!doc) {
                return 0;
            }

            /* Exercise the tree instead of the retired renderers. */
            markdown_core_iter *iter = markdown_core_iter_new(doc);
            while (markdown_core_iter_next(iter) != MARKDOWN_CORE_EVENT_DONE) {
                markdown_core_node *node = markdown_core_iter_get_node(iter);
                (void)markdown_core_node_get_type(node);
                (void)markdown_core_node_get_literal(node);
            }
            markdown_core_iter_free(iter);

            markdown_core_node_free(doc);
        }
    }
    return 0;
}
