#include <stdint.h>
#include <stdlib.h>
#include <markdown_core.h>

int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

/* The whole input is Markdown, parsed as the one dialect: the dialect has no
 * switches, so there is no configuration prefix to fuzz. The facade parses,
 * the canonical dump reads every node and node-valued field, and both are
 * released. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    markdown_core_document *document;
    if (markdown_core_document_parse(data, size, &document) != MARKDOWN_CORE_OK) {
        return 0;
    }
    uint8_t *dump = NULL;
    size_t dump_length = 0;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), data, size, &dump, &dump_length) ==
        MARKDOWN_CORE_OK) {
        markdown_core_dump_free(dump);
    }
    markdown_core_document_free(document);
    return 0;
}
