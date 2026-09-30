#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <markdown_core.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* The shipped path: parse through the facade, then read every node and
 * node-valued field through the canonical dump, and release both. */
static void exercise(const uint8_t *markdown, size_t length) {
    markdown_core_error *error;
    markdown_core_document *document = markdown_core_document_parse(markdown, length, &error);
    if (!document) {
        return;
    }
    uint8_t *dump = NULL;
    size_t dump_length = 0;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), markdown, length, &dump,
                                    &dump_length, &error)) {
        markdown_core_dump_free(dump);
    }
    markdown_core_document_free(document);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

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

            exercise((const uint8_t *)markdown, markdown_size);
        }
    }
    return 0;
}
