#ifndef MARKDOWN_CORE_UTF8_H
#define MARKDOWN_CORE_UTF8_H

#include <stdint.h>
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

MARKDOWN_CORE_EXPORT
void markdown_core_utf8proc_case_fold(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

MARKDOWN_CORE_EXPORT
void markdown_core_utf8proc_encode_char(int32_t uc, markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_iterate(const uint8_t *str, bufsize_t str_len, int32_t *dst);

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_is_space(int32_t uc);

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_is_punctuation(int32_t uc);

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_is_punctuation_or_symbol(int32_t uc);

/* Flanking classes are disjoint. ASCII needs no Unicode table lookup; its
 * whitespace set includes FF but excludes VT, unlike several C ctype sets. */
typedef enum { MARKDOWN_CORE_CHAR_OTHER, MARKDOWN_CORE_CHAR_SPACE, MARKDOWN_CORE_CHAR_PUNCT } markdown_core_char_class;

static inline markdown_core_char_class markdown_core_utf8proc_classify(int32_t c) {
    if (c < 128) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            return MARKDOWN_CORE_CHAR_SPACE;
        }
        return ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'))
                   ? MARKDOWN_CORE_CHAR_PUNCT
                   : MARKDOWN_CORE_CHAR_OTHER;
    }
    if (markdown_core_utf8proc_is_space(c)) {
        return MARKDOWN_CORE_CHAR_SPACE;
    }
    return markdown_core_utf8proc_is_punctuation_or_symbol(c) ? MARKDOWN_CORE_CHAR_PUNCT : MARKDOWN_CORE_CHAR_OTHER;
}

int markdown_core_utf8proc_is_letter(int32_t uc);
int markdown_core_utf8proc_is_number(int32_t uc);
int markdown_core_utf8proc_is_mark(int32_t uc);
/* Append the anchors module's Unicode projection of a valid UTF-8 literal. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

#ifdef __cplusplus
}
#endif

#endif
