#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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
        uint8_t splitpoint;
        uint8_t repeatlen;
    } fuzz_config;

    if (size >= sizeof(fuzz_config)) {
        /* The beginning of `data` is treated as fuzzer configuration */
        memcpy(&fuzz_config, data, sizeof(fuzz_config));

        /* Test options that are used by GitHub. */
        fuzz_config.options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_VALIDATE_UTF8;

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
                memcpy(
                    &markdown[markdown_size],
                    &markdown0[fuzz_config.splitpoint + fuzz_config.repeatlen],
                    size_after_splitpoint
                );
                markdown_size += size_after_splitpoint;
            } else {
                markdown_size = markdown_size0;
                memcpy(markdown, markdown0, markdown_size);
            }

            markdown_core_parser *parser = markdown_core_parser_new(fuzz_config.options);

            for (const char **it = extension_names; *it; ++it) {
                const char *extension_name = *it;
                markdown_core_extension *extension = markdown_core_extension_find(extension_name);
                if (!extension) {
                    fprintf(stderr, "%s is not a valid syntax extension\n", extension_name);
                    abort();
                }
                markdown_core_parser_attach_extension(parser, extension);
            }

            markdown_core_parser_feed(parser, markdown, markdown_size);
            markdown_core_node *doc = markdown_core_parser_finish(parser);

            /* Exercise the tree instead of the retired renderers. */
            markdown_core_iter *iter = markdown_core_iter_new(doc);
            while (markdown_core_iter_next(iter) != MARKDOWN_CORE_EVENT_DONE) {
                markdown_core_node *node = markdown_core_iter_get_node(iter);
                (void)markdown_core_node_get_type(node);
                (void)markdown_core_node_get_literal(node);
            }
            markdown_core_iter_free(iter);

            markdown_core_node_free(doc);
            markdown_core_parser_free(parser);
        }
    }
    return 0;
}
