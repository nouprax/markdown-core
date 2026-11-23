#ifndef MARKDOWN_CORE_ELEMENT_CITATION_H
#define MARKDOWN_CORE_ELEMENT_CITATION_H
#include "inlines.h"
#include "citation_state.h"
struct bracket;
void markdown_core_inline_free_citation_tokens(markdown_core_inline_state *inline_state, citation_tokens *tokens);
markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_state *inline_state);
/* A Citation at the end of `cite`'s citations, which are its children. */
markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_state *inline_state,
                                                      markdown_core_node *cite);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CITATION;
/* Resolve the tokens read in a bracket's body, with `citation` the
 * citation element's instance. */
void markdown_core_inline_finish_citation_tokens(const markdown_core_element_instance *citation,
                                                 markdown_core_inline_state *inline_state, citation_tokens *tokens);

#endif
