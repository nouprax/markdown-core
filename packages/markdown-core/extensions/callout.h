#ifndef MARKDOWN_CORE_EXT_CALLOUT_H
#define MARKDOWN_CORE_EXT_CALLOUT_H
#include "inlines.h"
#include "block_internal.h"
bool markdown_core_block_parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input);
bool markdown_core_callout_accepts_lazy_body(markdown_core_parser *parser, markdown_core_node *node);
bool markdown_core_callout_open_lazy_body(markdown_core_parser *parser);
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_CALLOUT;
#endif
