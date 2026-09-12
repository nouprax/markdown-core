#ifndef MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#define MARKDOWN_CORE_ELEMENT_DEFINITION_LIST_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_block_definition_body_blank_continues(markdown_core_parser *parser, markdown_core_node *body);
bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST;
void markdown_core_definition_list_close_body(markdown_core_node *node);
void markdown_core_definition_list_complete(markdown_core_node *node);
#endif
