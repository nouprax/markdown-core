#ifndef MARKDOWN_CORE_ELEMENT_LIST_H
#define MARKDOWN_CORE_ELEMENT_LIST_H
#include "inlines.h"
#include "block_internal.h"
void markdown_core_block_finalize_list(const markdown_core_parser *parser, markdown_core_node *list);
bool markdown_core_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                 markdown_core_chunk *input, const markdown_core_node *joining, bool *taken);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_LIST;

/* THE LIST MARKER GRAMMAR'S WORK (the list element's parse record):
 * the marker and ordinal bytes it examined, for its complexity gate. */
typedef struct {
    size_t markers;
} markdown_core_list_work;

#endif
