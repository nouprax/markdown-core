#ifndef MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#define MARKDOWN_CORE_ELEMENT_SPECIMEN_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN;

#endif
