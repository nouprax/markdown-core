#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
/* The headings' inline content, parsed in the parse's preparation
 * (element.h, markdown_core_stage_ops). */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser);
/* The attribute tails a heading's last line, which the content's end cuts
 * short, may hold, to the run's context sink; `declares` when the line is
 * a heading's only if the document finishes it as one. */
void markdown_core_heading_report_tail(markdown_core_inline_state *inline_state, bool declares);
void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent);
bool markdown_core_heading_claim_tail(const markdown_core_element_instance *self,
                                      markdown_core_inline_state *inline_state, markdown_core_node *parent);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;
#endif
