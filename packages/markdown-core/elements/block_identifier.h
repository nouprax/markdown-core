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

void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *parent,
                                                     markdown_core_node *paragraph);
/* A list item that takes its first child whole, the paragraph that gave
 * the `old` item it continues its anchor when it closed, has that anchor.
 * False when storage runs out. */
bool markdown_core_block_take_item_identifier(markdown_core_parser *parser, markdown_core_node *item,
                                              const markdown_core_node *old);
bool markdown_core_block_attach_identifier_line(markdown_core_block_identifier_work *work, markdown_core_parser *parser,
                                                markdown_core_node *parent, markdown_core_chunk *input);
#endif
