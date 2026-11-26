#ifndef MARKDOWN_CORE_ELEMENT_CITATION_H
#define MARKDOWN_CORE_ELEMENT_CITATION_H
#include "inlines.h"
#include "citation_state.h"
markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_state *inline_state);
/* A Citation at the end of `cite`'s citations, which are its children. */
markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_state *inline_state,
                                                      markdown_core_node *cite);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CITATION;

#endif
