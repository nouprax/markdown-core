#ifndef MARKDOWN_CORE_CROSS_LINK_H
#define MARKDOWN_CORE_CROSS_LINK_H
#include "parser.h"
#include "markdown-core-elements.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CROSS_LINK;

/* Both cross kinds own the same raw reference fields in one payload allocation.
 * Only CrossEmbed allocates the dimension value beside those fields. */
static inline markdown_core_cross_reference *markdown_core_node_cross_reference(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CROSS_LINK ? node->as.cross_link : &node->as.cross_embed->reference;
}

/* THE CROSS-LINK SCANNER'S WORK (the cross-link element's parse record): the
 * bytes it inspected, and the dimension-grammar work of its embeds, for its
 * complexity gates. */
typedef struct {
    size_t scan, dimensions;
} markdown_core_cross_link_work;

#ifdef __cplusplus
}
#endif
#endif
