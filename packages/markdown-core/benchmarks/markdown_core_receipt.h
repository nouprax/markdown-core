/* THE ROOT'S CHILD COUNT A MARKDOWN CORE RECEIPT CARRIES, as the canonical
 * dump states it: the `children=N` that ends the dump's first line, the
 * root's own. The dump is the facade call this count needs that both
 * revisions a gate compares have -- the navigation calls change between
 * them -- so one harness builds against both. A runner reads it after its
 * measured edges, so it is in no figure.
 */
#ifndef MARKDOWN_CORE_BENCH_RECEIPT_H
#define MARKDOWN_CORE_BENCH_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <markdown_core.h>

static inline bool bench_root_children(const markdown_core_document *document, const uint8_t *source, size_t length,
                                       size_t *children) {
    static const char FIELD[] = " children=";
    uint8_t *dump = NULL;
    size_t size = 0, line = 0, at;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), source, length, &dump, &size) !=
        MARKDOWN_CORE_OK) {
        return false;
    }
    while (line < size && dump[line] != '\n') {
        line++;
    }
    for (at = line; at >= sizeof(FIELD) - 1 && memcmp(dump + at - (sizeof(FIELD) - 1), FIELD, sizeof(FIELD) - 1);) {
        at--;
    }
    *children = 0;
    bool found = at >= sizeof(FIELD) - 1 && at < line;
    for (; found && at < line; at++) {
        *children = *children * 10 + (size_t)(dump[at] - '0');
    }
    markdown_core_dump_free(dump);
    return found;
}

#endif
