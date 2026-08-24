#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_node *node;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_mem *mem;
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
};

/* Consolidation with the region set kept in step: the survivor of each merged
 * text run takes the regions the nodes it absorbed owned (requirement 11b).
 * `markdown_core_consolidate_text_nodes` is this with no parser. */
int markdown_core_consolidate_text_nodes_with_parser(struct markdown_core_parser *parser, markdown_core_node *root);

#ifdef __cplusplus
}
#endif

#endif
