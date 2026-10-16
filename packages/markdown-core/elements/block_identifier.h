#ifndef MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#define MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#include "inlines.h"
#include "paragraph.h"
/* THE BLOCK-IDENTIFIER SUFFIX SCANNER'S WORK: the bytes it examined, for its
 * complexity gate. The scanner is the paragraph's, which attaches every
 * identifier, so this is the paragraph element's parse record; the calls
 * below count into the one they are given. */
typedef struct {
    size_t scan;
} markdown_core_block_identifier_work;

/* `paragraph` closes inside `parent`, which it is not yet a child of: an
 * identifier at its end names it, or the list item it opens. */
void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *paragraph,
                                                     markdown_core_node *parent);
bool markdown_core_block_attach_identifier_line(markdown_core_block_identifier_work *work, markdown_core_parser *parser,
                                                markdown_core_node *parent, markdown_core_chunk *input);
#endif
