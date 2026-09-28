#ifndef MARKDOWN_CORE_CROSS_LINK_H
#define MARKDOWN_CORE_CROSS_LINK_H
#include "parser.h"
#include "markdown-core-elements.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CROSS_LINK;

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
