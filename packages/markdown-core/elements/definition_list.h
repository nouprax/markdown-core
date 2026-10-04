#ifndef MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#define MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST;

/* THE DEFINITION LIST GRAMMAR'S WORK (the definition list
 * element's parse record): the marker and term bytes it examined, for its gate. */
typedef struct {
    size_t work;
} markdown_core_definition_list_work;

void markdown_core_definition_list_close_body(markdown_core_node *node);
void markdown_core_definition_list_complete(const markdown_core_parser *parser, markdown_core_node *node);
#endif
