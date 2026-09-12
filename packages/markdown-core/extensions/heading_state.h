#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "references.h"
/* A heading is registered once when its block closes. Source order is settled
 * before resolution, independently of the order in which mapped inputs close.
 * Pending holds the ordinary inline cursor at its declaration dependency;
 * nodes and resources remain owned by the tree and reference map. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_parser *pending;
    markdown_core_resource *resource;
} markdown_core_heading_parse;

typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity;
} markdown_core_heading_collection;

typedef struct {
    markdown_core_key_index index, resources;
} anchor_registry;
#endif
