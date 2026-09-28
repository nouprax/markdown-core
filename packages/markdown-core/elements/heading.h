#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
void markdown_core_block_register_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *node);
/* The heading lifecycle the document element drives (document.c). */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_observe(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node *node);
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_dispose(const markdown_core_element_instance *self);
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
