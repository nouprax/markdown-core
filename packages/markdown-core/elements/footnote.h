#ifndef MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#define MARKDOWN_CORE_ELEMENT_FOOTNOTE_H
#include "inlines.h"
#include "block_internal.h"
#include "bracket.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE;
/* The space of footnote definitions' labels (roster.h), folded. */
extern const markdown_core_space MARKDOWN_CORE_FOOTNOTE_SPACE;
/* An inline note's bracket, opened by a `^[`: a body of its own
 * (bracket.h). */
extern const markdown_core_bracket_shape MARKDOWN_CORE_FOOTNOTE_BRACKET;
bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_reader *reader);

#endif
