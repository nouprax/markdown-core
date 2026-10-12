#ifndef MARKDOWN_CORE_ELEMENT_HEADING_H
#define MARKDOWN_CORE_ELEMENT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
#include "heading_state.h"
/* The heading lifecycle the document element drives (document.c). */
void markdown_core_headings_init(const markdown_core_element_instance *self);
void markdown_core_headings_observe(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node *node);
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser);
void markdown_core_headings_dispose(const markdown_core_element_instance *self);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING;

#endif
