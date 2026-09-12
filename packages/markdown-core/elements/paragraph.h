#ifndef MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#define MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH;
markdown_core_node *markdown_core_paragraph_open_text(markdown_core_parser *parser, markdown_core_node *container,
                                                      markdown_core_chunk *input);
/* Finalize the semantics of an already positioned, attached paragraph whose
 * content retains its normalized line terminators. Definition-only paragraphs
 * stay attached until the block phase finishes processing identifiers. */
void markdown_core_parser_finalize_paragraph(struct markdown_core_parser *parser, struct markdown_core_node *node);

#endif
