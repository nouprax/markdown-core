#ifndef MARKDOWN_CORE_ELEMENT_SPAN_H
#define MARKDOWN_CORE_ELEMENT_SPAN_H
#include "bracket.h"
/* The span grammar is one of the link element's rules for a `]`
 * (link.c): a bracket whose `]` attributes follow is a span. */
markdown_core_bracket_match markdown_core_span_close(markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, struct bracket *opener);
#endif
