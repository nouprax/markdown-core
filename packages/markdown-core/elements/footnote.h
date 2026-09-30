#ifndef MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#define MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#include "inlines.h"
#include "block_internal.h"
struct bracket;
markdown_core_node *markdown_core_inline_close_inline_footnote(const markdown_core_element_instance *footnote,
                                                               markdown_core_parser *parser,
                                                               markdown_core_inline_state *inline_state,
                                                               struct bracket *opener);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE;
bool markdown_core_footnote_close_reference(const markdown_core_element_instance *footnote,
                                            markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                            struct bracket *opener);
bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input);
/* The footnote lifecycle the document element drives (document.c). */
void markdown_core_footnotes_begin(const markdown_core_element_instance *self, markdown_core_parser *parser);
bool markdown_core_footnotes_lost(const markdown_core_element_instance *self);
void markdown_core_footnotes_dispose(const markdown_core_element_instance *self);

/* THE FOOTNOTES OF ONE PARSE (the footnote element's parse record): the
 * labels the document defines -- the block phase fills it as each definition
 * opens and the inline phase reads it to decide whether a `[^label]` is a
 * call at all. */
typedef struct {
    struct markdown_core_map *labels;
} markdown_core_footnote_state;

#endif
