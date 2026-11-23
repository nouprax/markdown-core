#ifndef MARKDOWN_CORE_ELEMENT_ESCAPE_H
#define MARKDOWN_CORE_ELEMENT_ESCAPE_H
#include "parser.h"
#include "element.h"
#include "buffer.h"

/* THE BACKSLASH ESCAPES (CommonMark 0.31.2, 2.4, and 6.7 for a backslash
 * before a line ending): a backslash before an ASCII punctuation character
 * makes it a literal, one before a line ending is a hard break, and one
 * before a space is a word space (services.h). */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ESCAPE;

/* THE ESCAPE ELEMENT'S WORK (its parse record): the lookahead bytes of the
 * white space after an escaped space, for its gate. */
typedef struct {
    size_t whitespace;
} markdown_core_escape_work;

/* It provides MARKDOWN_CORE_SERVICE_ESCAPES (services.h), which strips a
 * slice's escapes in place. */

#endif
