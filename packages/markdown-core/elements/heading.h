#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
/* The heading lifecycle the document element drives (document.c). */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_dispose(const markdown_core_element_instance *self);
/* The headings of a flushed parse that start before `start`, which an edit
 * reopens there: the others go aside, and the next finish names them all
 * again. Each that stays is listed as committed (ast.c, A DOCUMENT REOPENED
 * FOR AN EDIT). False when storage could not be had. */
bool markdown_core_headings_reopen(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   uint32_t start);
/* The headings the reopen put aside that start at `start` or after, back,
 * moved to start from `point` on, and listed as committed; the others go
 * (blocks.c, THE SPLICE). False when storage could not be had. */
bool markdown_core_headings_splice(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   uint32_t start, uint32_t point);
/* The attribute tails a heading's last line, which the content's end cuts
 * short, may hold, to the run's context sink; `declares` when the line is
 * a heading's only if the document finishes it as one. */
void markdown_core_heading_report_tail(markdown_core_inline_state *inline_state, bool declares);
void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent);
bool markdown_core_heading_claim_tail(const markdown_core_element_instance *self,
                                      markdown_core_inline_state *inline_state, markdown_core_node *parent);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;
void markdown_core_prepare_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_heading_parse *heading);
void markdown_core_finish_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading);
void markdown_core_dispose_heading(markdown_core_heading_parse *heading);

#endif
