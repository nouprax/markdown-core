#ifndef MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#define MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#include "inlines.h"
#include "block_internal.h"
struct bracket;
markdown_core_node *markdown_core_inline_close_inline_footnote(markdown_core_parser *parser,
                                                               markdown_core_inline_state *inline_state,
                                                               struct bracket *opener);
void markdown_core_block_finalize_footnotes(markdown_core_parser *parser);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE;
bool markdown_core_footnote_close_reference(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                            struct bracket *opener);
bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
#endif
