#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
void markdown_core_block_register_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *node);
/* The heading lifecycle the document element drives (document.c). */
/* Every heading's inlines, and the label each declares. */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser);
/* Gives each heading its anchor and its target's destination, then
 * resolves the round (registry.h). */
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_dispose(const markdown_core_element_instance *self);
void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent);
bool markdown_core_heading_claim_tail(const markdown_core_element_instance *self,
                                      markdown_core_inline_state *inline_state, markdown_core_node *parent);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;

#endif
