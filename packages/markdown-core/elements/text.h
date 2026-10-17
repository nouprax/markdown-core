#ifndef MARKDOWN_CORE_ELEMENT_TEXT_H
#define MARKDOWN_CORE_ELEMENT_TEXT_H
#include "parser.h"
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_TEXT;

/* The text element's flag, on a Text: a contextual escape token, decoded
 * once inline ownership is final. */
enum { MARKDOWN_CORE_NODE__ESCAPED_SPACE = MARKDOWN_CORE_NODE__ELEMENT_FIRST };

/* THE TEXT ELEMENT'S WORK (the text element's parse record): ordinary
 * whitespace scalars and contextual-space lookahead bytes, for its gate. */
typedef struct {
    size_t whitespace;
} markdown_core_text_work;

#endif
