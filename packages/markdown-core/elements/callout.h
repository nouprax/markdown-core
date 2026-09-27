#ifndef MARKDOWN_CORE_ELEMENT_CALLOUT_H
#define MARKDOWN_CORE_ELEMENT_CALLOUT_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_block_parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT;
#endif
