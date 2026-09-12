#ifndef MARKDOWN_CORE_EXT_CITATION_H
#define MARKDOWN_CORE_EXT_CITATION_H
#include "inlines.h"
#include "citation_state.h"
struct bracket;
void markdown_core_inline_free_citation_tokens(markdown_core_inline_parser *inline_parser, citation_tokens *tokens);
markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_parser *inline_parser);
markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_parser *inline_parser,
                                                      markdown_core_node *cite, markdown_core_node *last);
void markdown_core_inline_finish_citation_tokens(markdown_core_inline_parser *inline_parser, citation_tokens *tokens);
bool markdown_core_inline_close_bibliography(markdown_core_parser *parser, markdown_core_inline_parser *inline_parser,
                                             struct bracket *opener);
bool markdown_core_citation_defer_tail(markdown_core_inline_parser *inline_parser, struct bracket *opener,
                                       markdown_core_node **result);
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_CITATION;
void markdown_core_citation_open_bracket(markdown_core_inline_parser *inline_parser, struct bracket *b);
#endif
