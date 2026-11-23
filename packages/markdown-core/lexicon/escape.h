#ifndef MARKDOWN_CORE_LEXICON_ESCAPE_H
#define MARKDOWN_CORE_LEXICON_ESCAPE_H

#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE BACKSLASH ESCAPES (CommonMark 0.31.2, 2.4), a lexical rule several
 * elements share: `buf` loses each backslash before an ASCII punctuation
 * character, in place. */
void markdown_core_strbuf_unescape(markdown_core_strbuf *buf);

#ifdef __cplusplus
}
#endif

#endif
