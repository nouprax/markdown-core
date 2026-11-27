#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;
#endif
