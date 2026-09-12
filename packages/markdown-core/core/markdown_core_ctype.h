#ifndef MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H
#define MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core-export.h"

/** Locale-independent versions of functions from ctype.h.
 * We want markdown_core to behave the same no matter what the system locale.
 */

MARKDOWN_CORE_EXPORT
int markdown_core_isspace(char c);

MARKDOWN_CORE_EXPORT
int markdown_core_ispunct(char c);

MARKDOWN_CORE_EXPORT
int markdown_core_isalnum(char c);

MARKDOWN_CORE_EXPORT
int markdown_core_isdigit(char c);

MARKDOWN_CORE_EXPORT
int markdown_core_isalpha(char c);

#ifdef __cplusplus
}
#endif

/* Source-line boundaries use ASCII CR/LF in every syntax scanner. */
static inline int markdown_core_is_line_end(unsigned char c) { return c == '\n' || c == '\r'; }

#endif
