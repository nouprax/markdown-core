#ifndef MARKDOWN_CORE_ELEMENT_CITATION_H
#define MARKDOWN_CORE_ELEMENT_CITATION_H
#include "inlines.h"
#include "citation_state.h"
struct bracket;
void markdown_core_inline_free_citation_tokens(markdown_core_inline_state *inline_state, citation_tokens *tokens);
markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_state *inline_state);
/* A Citation at the end of `cite`'s items, held there; NULL, with the run
 * failed, when it cannot be made or placed. */
markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_state *inline_state,
                                                      markdown_core_node *cite);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CITATION;
/* The citation grammar's calls from the bracket algorithm, with `citation`
 * the citation element's instance. Tokens exist only where that element read
 * them, so a dialect without it has none to finish or open. */
void markdown_core_inline_finish_citation_tokens(const markdown_core_element_instance *citation,
                                                 markdown_core_inline_state *inline_state, citation_tokens *tokens);
/* Resolve the run's citation tokens read outside every bracket. */
void markdown_core_citation_finish_run_tokens(const markdown_core_element_instance *citation,
                                              markdown_core_inline_state *inline_state);
bool markdown_core_inline_close_bibliography(const markdown_core_element_instance *citation,
                                             markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                             struct bracket *opener);
bool markdown_core_citation_defer_tail(const markdown_core_element_instance *citation,
                                       markdown_core_inline_state *inline_state, struct bracket *opener,
                                       markdown_core_node **result);
void markdown_core_citation_open_bracket(const markdown_core_element_instance *citation,
                                         markdown_core_inline_state *inline_state, struct bracket *b);
#endif
