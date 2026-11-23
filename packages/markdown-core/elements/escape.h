#ifndef MARKDOWN_CORE_ELEMENT_ESCAPE_H
#define MARKDOWN_CORE_ELEMENT_ESCAPE_H
#include "parser.h"
#include "element.h"
#include "buffer.h"

/* THE BACKSLASH ESCAPES (CommonMark 0.31.2, 2.4, and 6.7 for a backslash
 * before a line ending): a backslash before an ASCII punctuation character
 * makes it a literal, one before a line ending is a hard break, and one
 * before a space is the contextual space the text element decodes
 * (text.h). */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ESCAPE;

/* THE ESCAPE ELEMENT'S WORK (its parse record): the lookahead bytes of the
 * white space after an escaped space, for its gate. */
typedef struct {
    size_t whitespace;
} markdown_core_escape_work;

/* The escapes of a slice another element decodes (a link's destination and
 * title, a fence's info string), for an element that holds the escape
 * element as a peer: `buf` loses each backslash before an ASCII punctuation
 * character, in place. A NULL `escape`, a dialect without the element,
 * escapes nothing, and `buf` is left as it is. */
void markdown_core_escape_strip(const markdown_core_element_instance *escape, markdown_core_strbuf *buf);

#endif
