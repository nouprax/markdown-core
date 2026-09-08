#ifndef MARKDOWN_CORE_METADATA_H
#define MARKDOWN_CORE_METADATA_H

#include "../include/markdown_core.h"
#include "markdown-core.h"

/* Only the union member selected by kind is active; its allocations belong to
 * the document. Readers and cleanup must not inspect the inactive member. */
struct markdown_core_metadata_value {
    /* Zero means absent; no union member is active in that state. */
    markdown_core_metadata_value_kind kind;
    union {
        markdown_core_metadata_scalar scalar;
        struct {
            markdown_core_metadata_list_item *items;
            size_t count;
        } list;
    } as;
};

/* Named values are inline and owned by the document. No authored field order
 * or per-field source position is retained. */
struct markdown_core_metadata {
    markdown_core_scope scope;
    markdown_core_metadata_value name;
    markdown_core_metadata_value title;
    markdown_core_metadata_value subtitle;
    markdown_core_metadata_value time;
    markdown_core_metadata_value date;
    markdown_core_metadata_value authors;
    markdown_core_metadata_value keywords;
    markdown_core_metadata_value abstract;
    markdown_core_metadata_value state;
    markdown_core_metadata_value comment;
};

/* The document owns the committed result; decoder temporaries use the same allocator. */
void markdown_core_metadata_free(markdown_core_mem *mem, markdown_core_metadata *metadata);

size_t markdown_core_metadata_parse(markdown_core_parser *parser, const unsigned char *source, size_t length);

#endif
