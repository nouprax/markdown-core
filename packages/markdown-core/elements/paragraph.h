#ifndef MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#define MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH;
/* Finalize the semantics of an already positioned, attached paragraph whose
 * content retains its normalized line terminators: the paragraph element's
 * own `finalize_block`, with `paragraph` its instance. Definition-only
 * paragraphs stay attached until the block phase finishes processing
 * identifiers. */
void markdown_core_paragraph_finalize(const markdown_core_element_instance *paragraph, markdown_core_parser *parser,
                                      markdown_core_node *node);

#endif
