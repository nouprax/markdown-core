#ifndef MARKDOWN_CORE_ELEMENT_SPAN_H
#define MARKDOWN_CORE_ELEMENT_SPAN_H
#include "inline_internal.h"
struct bracket;
markdown_core_bracket_match markdown_core_span_close(markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, struct bracket *opener);
#endif
