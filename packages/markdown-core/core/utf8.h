#ifndef MARKDOWN_CORE_UTF8_H
#define MARKDOWN_CORE_UTF8_H

#include <stdint.h>
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Append the reference-label normal form of `str`: case folded, with leading
 * and trailing whitespace dropped and each interior run collapsed to a single
 * space. */
void markdown_core_utf8proc_normalize_label(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

MARKDOWN_CORE_EXPORT
void markdown_core_utf8proc_encode_char(int32_t uc, markdown_core_strbuf *buf);

/* DECODE ONE CHARACTER, whatever its width, where the compiler can see it.
 *
 * Returns the width of the well-formed character that starts at `str` and
 * writes its scalar through `dst`, or returns -1 and writes -1 when none
 * starts there: an empty or negative range, a continuation byte, a lead byte
 * no well-formed sequence begins with (0xC0, 0xC1, 0xF5 and up), a sequence
 * cut short by the range or by a byte that does not continue it, an overlong
 * form, a surrogate, or a scalar past U+10FFFF. `bufsize_t` is a SIGNED
 * int32_t, which is why the range test is `len > 0` before any byte is read.
 *
 * Every width is decoded here, inline. The one-byte case is not a fast path
 * beside a general decoder: a byte below 0x80 is a complete scalar equal to
 * its own value, which is the first line of UTF-8's definition. The longer
 * forms take their payload from the lead byte and six bits from each
 * continuation byte, one byte at a time, so a character costs what its bytes
 * do. They used to be decoded by an out-of-line function, which charged every
 * character of a non-ASCII script a call on top of its bytes; at 44, 58 and
 * 70 instructions for two-, three- and four-byte characters it was most of
 * what a heading anchor cost outside ASCII (#405).
 *
 * Decoding is NOT skippable in general. `markdown_core_utf8proc_is_space`
 * matches the Zs class, every non-control member of which (160, 5760,
 * 8192-8202, 8239, 8287, 12288) is at or above 128. Walks that only need to
 * know where a character ends read its width off the lead byte instead
 * (`markdown_core_utf8proc_width`). */
static inline int markdown_core_utf8proc_iterate(const uint8_t *str, bufsize_t len, int32_t *dst) {
    uint32_t lead, scalar, least;
    int width;
    if (len <= 0) {
        *dst = -1;
        return -1;
    }
    lead = str[0];
    if (lead < 0x80) {
        *dst = (int32_t)lead;
        return 1;
    }
    if (lead < 0xC2 || lead > 0xF4) {
        *dst = -1;
        return -1;
    }
    width = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    least = width == 2 ? 0x80 : width == 3 ? 0x800 : 0x10000;
    if (width > len) {
        *dst = -1;
        return -1;
    }
    scalar = lead & (0x7Fu >> width);
    for (int i = 1; i < width; i++) {
        uint32_t next = str[i];
        if ((next & 0xC0) != 0x80) {
            *dst = -1;
            return -1;
        }
        scalar = (scalar << 6) | (next & 0x3F);
    }
    if (scalar < least || (scalar >= 0xD800 && scalar < 0xE000) || scalar > 0x10FFFF) {
        *dst = -1;
        return -1;
    }
    *dst = (int32_t)scalar;
    return width;
}

/* THE WIDTH OF THE CHARACTER THAT `lead` BEGINS, read off the byte alone:
 * 0xxxxxxx is one byte, 110xxxxx two, 1110xxxx three and 11110xxx four.
 *
 * For walks that segment text into characters without asking what any of
 * them is -- a grid column per character, a task marker that is one
 * character. Those need no scalar, so they decode nothing: the width is one
 * load for every lead byte, and on valid UTF-8 it is exactly the width
 * `markdown_core_utf8proc_iterate` reports.
 *
 * Valid UTF-8 is a caller precondition (markdown_core.h). On other bytes the
 * answer is still between one and four -- a continuation byte counts as a
 * character of its own -- but a lead byte cut off by the end of its range
 * claims bytes the range does not have. Only the byte is read here, so the
 * range is the caller's to bound: a walk ends when it reaches OR PASSES its
 * end, and a probe checks that the width fits before reading past it. */
static inline int markdown_core_utf8proc_width(uint8_t lead) {
    /* Indexed by the high four bits: 0-7 begin one-byte characters, 8-B are
     * continuation bytes, C-D, E and F lead two, three and four. */
    static const uint8_t widths[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4};
    return widths[lead >> 4];
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

/* THE UNICODE CLASSES THE GRAMMAR ASKS ABOUT, one bit each, from Unicode 17
 * (docs/specs/dialect.md). A scalar may have several: every ASCII
 * punctuation character is punctuation, and those whose category is S are
 * symbols too. */
enum {
    /* Zs, and tab, LF, form feed and CR: CommonMark's Unicode whitespace. */
    MARKDOWN_CORE_UNICODE_SPACE = 1 << 0,
    /* P, and every ASCII punctuation character. */
    MARKDOWN_CORE_UNICODE_PUNCTUATION = 1 << 1,
    MARKDOWN_CORE_UNICODE_SYMBOL = 1 << 2,
    MARKDOWN_CORE_UNICODE_LETTER = 1 << 3,
    MARKDOWN_CORE_UNICODE_NUMBER = 1 << 4
};

/* The staged class table unicode_categories.inc defines, generated from
 * Unicode 17 by scripts/tooling/generate-unicode-categories.mjs. */
extern const uint8_t markdown_core_unicode_pages[];
extern const uint8_t markdown_core_unicode_blocks[][128];

/* The classes of a scalar, 0 for none and for anything that is not a scalar.
 * Two dependent loads whatever the scalar's script: every predicate below is
 * one lookup and one test, and a caller asking two questions of one scalar
 * pays for one lookup. */
static inline uint8_t markdown_core_utf8proc_classes(int32_t uc) {
    if ((uint32_t)uc >= 0x110000) {
        return 0;
    }
    return markdown_core_unicode_blocks[markdown_core_unicode_pages[uc >> 7]][uc & 127];
}

static inline int markdown_core_utf8proc_is_space(int32_t uc) {
    return markdown_core_utf8proc_classes(uc) & MARKDOWN_CORE_UNICODE_SPACE;
}

/* Punctuation as CommonMark 0.29 and GFM define it: ASCII punctuation and P. */
static inline int markdown_core_utf8proc_is_punctuation(int32_t uc) {
    return markdown_core_utf8proc_classes(uc) & MARKDOWN_CORE_UNICODE_PUNCTUATION;
}

/* A Unicode punctuation character as CommonMark 0.31 defines it: P or S. */
static inline int markdown_core_utf8proc_is_punctuation_or_symbol(int32_t uc) {
    return markdown_core_utf8proc_classes(uc) & (MARKDOWN_CORE_UNICODE_PUNCTUATION | MARKDOWN_CORE_UNICODE_SYMBOL);
}

static inline int markdown_core_utf8proc_is_letter(int32_t uc) {
    return markdown_core_utf8proc_classes(uc) & MARKDOWN_CORE_UNICODE_LETTER;
}

static inline int markdown_core_utf8proc_is_number(int32_t uc) {
    return markdown_core_utf8proc_classes(uc) & MARKDOWN_CORE_UNICODE_NUMBER;
}

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
