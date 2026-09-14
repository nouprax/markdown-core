#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "references.h"
/* A heading is registered once when its block closes. Source order is settled
 * before resolution, independently of the order in which mapped inputs close.
 * Pending holds the ordinary inline cursor at its declaration dependency;
 * nodes and the record's resource remain owned by the tree and reference map. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
    markdown_core_map_record *record;
} markdown_core_heading_parse;

typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity;
    /* The anchor projection stack every heading reuses (heading.c). */
    void *projection_stack;
    size_t projection_capacity;
} markdown_core_heading_collection;

typedef struct {
    markdown_core_key_index index, resources;
} anchor_registry;
#endif
