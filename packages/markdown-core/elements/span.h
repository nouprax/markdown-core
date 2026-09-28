#ifndef MARKDOWN_CORE_ELEMENT_SPAN_H
#define MARKDOWN_CORE_ELEMENT_SPAN_H
#include "bracket_state.h"
struct bracket;
/* The span grammar is the bracket algorithm's, so it closes `link`'s
 * bracket; `citation` is the citation element's instance or NULL. */
markdown_core_bracket_match markdown_core_span_close(const markdown_core_element_instance *link,
                                                     const markdown_core_element_instance *citation,
                                                     markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, struct bracket *opener);
#endif
