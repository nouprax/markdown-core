#ifndef MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#define MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#include "inlines.h"
#include "block_internal.h"
void markdown_core_block_prepare_specimens(markdown_core_parser *parser);
bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN;
void markdown_core_specimen_finish(markdown_core_parser *parser);
#endif
