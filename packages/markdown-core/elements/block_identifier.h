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
                                                     markdown_core_parser *parser, markdown_core_member *paragraph);
/* Whether a block of `node`'s kind takes an identifier line after it: a
 * closed one awaits that trailer (`awaits_trailer`) as its owner's last
 * block. */
bool markdown_core_block_identifier_trails(const markdown_core_node *node);
bool markdown_core_block_attach_identifier_line(markdown_core_block_identifier_work *work, markdown_core_parser *parser,
                                                markdown_core_member *parent, markdown_core_chunk *input);
#endif
