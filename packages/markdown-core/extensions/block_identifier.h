#ifndef MARKDOWN_CORE_EXT_BLOCK_IDENTIFIER_H
#define MARKDOWN_CORE_EXT_BLOCK_IDENTIFIER_H
#include "inlines.h"
void markdown_core_block_attach_paragraph_identifier(markdown_core_parser *parser, markdown_core_node *paragraph);
bool markdown_core_block_attach_identifier_line(markdown_core_parser *parser, markdown_core_node *parent,
                                                markdown_core_chunk *input);
#endif
