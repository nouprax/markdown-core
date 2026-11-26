#ifndef MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#define MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_reader *reader);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST;

/* THE DEFINITION LIST GRAMMAR'S WORK (the definition list
 * element's parse record): the marker and term bytes it examined, for its gate. */
typedef struct {
    size_t work;
    /* The body whose continuation reads on past a blank line, while its
     * reading's look stands on the lines after it. */
    markdown_core_node *body;
} markdown_core_definition_list_work;

#endif
