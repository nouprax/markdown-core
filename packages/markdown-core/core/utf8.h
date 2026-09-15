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

int markdown_core_utf8proc_is_letter(int32_t uc);
int markdown_core_utf8proc_is_number(int32_t uc);
int markdown_core_utf8proc_is_mark(int32_t uc);
/* Append the anchors module's Unicode projection of a valid UTF-8 literal. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

#ifdef __cplusplus
}
#endif

#endif
