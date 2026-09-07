#ifndef MARKDOWN_CORE_METADATA_H
#define MARKDOWN_CORE_METADATA_H

#include "../include/markdown_core.h"

/* Only the union member selected by kind is active; its allocations belong to
 * the document. Readers and cleanup must not inspect the inactive member. */
typedef struct {
    markdown_core_metadata_value_kind kind;
    union {
        markdown_core_metadata_scalar scalar;
        struct {
            markdown_core_metadata_list_item *items;
            size_t count;
        } list;
    } as;
} markdown_core_metadata_value;

/* The document owns these values and every string/list allocation below them.
 * Facade strings borrow them. O6 is the first syntax producer. */
struct markdown_core_metadata_record {
    markdown_core_scope scope;
    markdown_core_string name;
    markdown_core_metadata_value value;
};

struct markdown_core_metadata {
    markdown_core_scope scope;
    markdown_core_metadata_record *records;
    size_t count;
};

#endif
