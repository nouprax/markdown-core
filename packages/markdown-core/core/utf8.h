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

/* DECODE AND ADVANCE, total.
 *
 * `markdown_core_utf8proc_iterate` answers a validating question -- "does a
 * well-formed character start here?" -- and reports no with a NEGATIVE return.
 * That answer has one legitimate caller, `is_valid_hostchar` in
 * elements/autolink.c, which decodes at every byte offset of a domain and uses
 * the rejection to end the scan.
 *
 * Every other caller is walking a buffer character by character from a known
 * boundary and only wants the width to advance by. For those, the negative
 * return is a trap: `bufsize_t` is a SIGNED int32_t, so `at += width` with -1
 * walks BACKWARDS, off the front of the buffer and around the loop condition.
 * Callers have been guarding it one at a time with `width > 0 ? width : 1`,
 * and `markdown_core_utf8proc_anchor` did not -- it asserted the precondition
 * instead, and Release is the default build type, so the assertion is compiled
 * out and a malformed byte in a heading hangs the parser reading below its
 * own buffer.
 *
 * Valid UTF-8 is a caller precondition (markdown_core.h) and this does not
 * repair anything: it decodes exactly as `iterate` does whenever `iterate`
 * succeeds, which on conforming input is always. It only makes the ADVANCE
 * total, so a buffer walk cannot be turned inside out by bytes the contract
 * already excludes. Progress is at least one byte and never past `len`.
 *
 * For ADVANCING walks only, which is to say callers whose loop condition
 * already guarantees a byte remains. A read-only probe that may sit AT the end
 * of its range wants `iterate`: its -1 means "no character here", and an empty
 * range is one of the ways that happens. `markdown_core_inline_scan_delimiter`
 * relies on exactly that -- the flanking skip can walk `after_char_pos` to
 * `input.len`, and the resulting -1 is what makes end-of-input read as a
 * newline. */
static inline int markdown_core_utf8proc_step(const uint8_t *str, bufsize_t len, int32_t *dst) {
    int width;
    if (len <= 0) {
        *dst = -1;
        return 1;
    }
    width = markdown_core_utf8proc_iterate(str, len, dst);
    if (width > 0) {
        return width;
    }
    /* Not a character here. Hand back the byte itself and step over it; what
     * such input parses to is not defined, only that the walk terminates. */
    *dst = (int32_t)str[0];
    return 1;
}

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
