#ifndef MARKDOWN_CORE_EXT_FOOTNOTE_H
#define MARKDOWN_CORE_EXT_FOOTNOTE_H
#include "inlines.h"
#include "block_internal.h"
struct bracket;
markdown_core_node *markdown_core_inline_close_inline_footnote(markdown_core_parser *parser,
                                                               markdown_core_inline_parser *inline_parser,
                                                               struct bracket *opener);
void markdown_core_block_finalize_footnotes(markdown_core_parser *parser);
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_FOOTNOTE;
bool markdown_core_footnote_close_reference(markdown_core_parser *parser, markdown_core_inline_parser *inline_parser,
                                            struct bracket *opener);
bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
#endif
