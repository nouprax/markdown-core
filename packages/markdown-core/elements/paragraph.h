#ifndef MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#define MARKDOWN_CORE_ELEMENT_PARAGRAPH_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH;
/* Finalize the semantics of a positioned paragraph closing inside `parent`,
 * whose content retains its normalized line terminators: the paragraph
 * element's own `finalize_block`, with `paragraph` its instance. Its
 * reference definitions settle into `parent` before it, and a paragraph that
 * held nothing else is marked to leave the tree. */
void markdown_core_paragraph_finalize(const markdown_core_element_instance *paragraph, markdown_core_parser *parser,
                                      markdown_core_node *node, markdown_core_node *parent);

#endif
