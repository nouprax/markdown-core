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

/* The general decoder. Callers do not name this one: they name
 * `markdown_core_utf8proc_iterate` below, which settles the single-byte case
 * itself and delegates everything else here. */
MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_iterate_general(const uint8_t *str, bufsize_t str_len, int32_t *dst);

/* DECODE ONE CHARACTER, with the single-byte case where the compiler can see
 * it.
 *
 * This is not an "ASCII path" -- this parser is UTF-8 and nothing else. It is
 * the one-byte branch of UTF-8 itself: a byte below 0x80 can be neither a
 * continuation byte (0x80-0xBF) nor a lead byte (0xC2+), so it is always a
 * complete one-byte scalar equal to its own value. utf8.c's own class table
 * says exactly that -- `utf8proc_utf8class[0..127]` is all 1s -- and the
 * switch in the general decoder then takes `case 1: uc = str[0]`. Identical
 * bit for bit, so this is not a fast path with its own semantics; it is the
 * same answer, reached without an out-of-line call.
 *
 * Decoding is NOT skippable in general. `markdown_core_utf8proc_is_space`
 * matches the Zs class, every non-control member of which (160, 5760,
 * 8192-8202, 8239, 8287, 12288) is at or above 128. What this removes is the
 * CALL on the bytes where the encoding has already settled the answer, which
 * is most of them.
 *
 * `bufsize_t` is a SIGNED int32_t, so the guard is `> 0`: a zero or negative
 * length falls through to the general decoder, which returns -1 and writes -1
 * through `dst`. Reading str[0] first would be a read out of bounds.
 *
 * The inline wrapper carries the name the callers use, rather than the general
 * decoder carrying it and a `_fast` variant sitting beside it. A call site
 * cannot then be written, or left behind by a rename, that misses this. */
static inline int markdown_core_utf8proc_iterate(const uint8_t *str, bufsize_t str_len, int32_t *dst) {
    if (str_len > 0 && str[0] < 0x80) {
        *dst = (int32_t)str[0];
        return 1;
    }
    return markdown_core_utf8proc_iterate_general(str, str_len, dst);
}

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

/* Anything in the Zs class, plus LF, CR, TAB, FF.
 *
 * Eleven comparisons on a scalar: no state, no table, no allocation. It was an
 * out-of-line call in another translation unit, which at nineteen call sites
 * cost more to reach than to evaluate. Nothing outside this library ever named
 * it -- not the export map, not a test, not a benchmark -- so there is no
 * second definition to drift from this one. */
static inline int markdown_core_utf8proc_is_space(int32_t uc) {
    return (uc == 9 || uc == 10 || uc == 12 || uc == 13 || uc == 32 || uc == 160 || uc == 5760 ||
            (uc >= 8192 && uc <= 8202) || uc == 8239 || uc == 8287 || uc == 12288);
}

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_is_punctuation(int32_t uc);

MARKDOWN_CORE_EXPORT
int markdown_core_utf8proc_is_punctuation_or_symbol(int32_t uc);

int markdown_core_utf8proc_is_letter(int32_t uc);
int markdown_core_utf8proc_is_number(int32_t uc);
int markdown_core_utf8proc_is_mark(int32_t uc);

/* THE WIDTH OF AN ALPHANUMERIC CHARACTER at `str`, or 0 when the character
 * there is not one (or `len` is 0). "Alphanumeric" is what Pandoc's grammar
 * means by it, a Unicode letter or number: the class that ends a name which
 * has no delimiter after it, such as a bare citation key, where `@张三，`
 * must key `张三` and not the clause. ASCII is decided on the byte; only a
 * byte at or above 0x80 decodes a scalar. */
static inline int markdown_core_utf8proc_alnum_width(const uint8_t *str, bufsize_t len) {
    int32_t scalar;
    int width;
    if (len <= 0) {
        return 0;
    }
    if (str[0] < 0x80) {
        return (str[0] >= '0' && str[0] <= '9') || ((str[0] | 0x20) >= 'a' && (str[0] | 0x20) <= 'z');
    }
    width = markdown_core_utf8proc_iterate(str, len, &scalar);
    return width > 0 && (markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar)) ? width
                                                                                                               : 0;
}
/* Append the anchors module's Unicode projection of a valid UTF-8 literal. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

#ifdef __cplusplus
}
#endif

#endif
