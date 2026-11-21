#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
/* The headings' inline content, parsed in the parse's preparation
 * (element.h, markdown_core_stage_ops). */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;
#endif
