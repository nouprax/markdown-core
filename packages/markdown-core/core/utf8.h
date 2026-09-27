#ifndef MARKDOWN_CORE_UTF8_H
#define MARKDOWN_CORE_UTF8_H

#include <assert.h>
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

/* A SCALAR NO CHARACTER HAS: the first value past Unicode's range. The class
 * lookup, the anchor projection, case folding and the front-matter printable
 * test all answer "none" for it, as for every value above it. */
enum { MARKDOWN_CORE_NO_SCALAR = 0x110000 };

/* THE CHARACTER THAT BEGINS AT `str`: returns its width and writes its scalar
 * through `dst`. `len` is at least 1. Every caller has a byte in hand, and
 * what the end of a range means is the caller's to say: beside a delimiter it
 * reads as a newline, after a citation key as no letter.
 *
 * Valid UTF-8 is a caller precondition (markdown_core.h), and on it this is
 * UTF-8's definition read off the bytes. The lead byte's high bits give the
 * width, the partition markdown_core_utf8proc_width tabulates. The scalar is
 * the lead's payload followed by six bits of each continuation byte. Every
 * continuation byte is 10xxxxxx and a lead of width w has w + 1 fixed high
 * bits, so shifting each byte into place and XORing one constant per width
 * clears exactly the tag bits: one shift and one XOR a byte, and no mask.
 *
 * Nothing is validated. No caller asks whether bytes are well formed, and a
 * check the contract makes unreachable would be paid by every character of
 * every script outside ASCII. What holds on ANY bytes is what memory safety
 * and termination need:
 * - only str[0 .. width - 1] is read;
 * - the width is min(markdown_core_utf8proc_width(str[0]), len), so a walk
 *   always moves forward and never past its range;
 * - the scalar is in [0, 0x400000).
 * A continuation byte where a character should begin, and a lead whose width
 * the range cannot hold, are characters with MARKDOWN_CORE_NO_SCALAR. A lead
 * followed by bytes that do not continue it still takes them. What input
 * that is not UTF-8 parses to is unspecified. */
static inline int markdown_core_utf8proc_decode(const uint8_t *str, bufsize_t len, int32_t *dst) {
    assert(len > 0);
    const uint32_t lead = str[0];
    if (lead < 0x80) {
        *dst = (int32_t)lead;
        return 1;
    }
    if (lead < 0xC0) {
        *dst = MARKDOWN_CORE_NO_SCALAR;
        return 1;
    }
    if (lead < 0xE0) {
        if (len >= 2) {
            *dst = (int32_t)((lead << 6) ^ str[1] ^ 0x3080u);
            return 2;
        }
    } else if (lead < 0xF0) {
        if (len >= 3) {
            *dst = (int32_t)((lead << 12) ^ ((uint32_t)str[1] << 6) ^ str[2] ^ 0xE2080u);
            return 3;
        }
    } else if (len >= 4) {
        *dst = (int32_t)((lead << 18) ^ ((uint32_t)str[1] << 12) ^ ((uint32_t)str[2] << 6) ^ str[3] ^ 0x3C82080u);
        return 4;
    }
    /* Cut off by the range: the rest of the range, as a width walk clamps. */
    *dst = MARKDOWN_CORE_NO_SCALAR;
    return (int)len;
}

/* THE WIDTH OF THE CHARACTER THAT `lead` BEGINS, read off the byte alone:
 * 0xxxxxxx is one byte, 110xxxxx two, 1110xxxx three and 11110xxx four.
 *
 * For walks that segment text into characters without asking what any of
 * them is -- a grid column per character, a task marker that is one
 * character. Those need no scalar, so they decode nothing: the width is one
 * load for every lead byte. It is the same model as
 * markdown_core_utf8proc_decode, whose width is exactly min(this, len).
 *
 * A continuation byte counts as a character of its own, and a lead cut off by
 * the end of its range claims bytes the range does not have. Only the byte is
 * read here, so the range is the caller's to bound: a walk ends when it
 * reaches OR PASSES its end, and a probe checks that the width fits before
 * reading past it. */
static inline int markdown_core_utf8proc_width(uint8_t lead) {
    /* Indexed by the high four bits: 0-7 begin one-byte characters, 8-B are
     * continuation bytes, C-D, E and F lead two, three and four. */
    static const uint8_t widths[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4};
    return widths[lead >> 4];
}

/* THE UNICODE CLASSES THE GRAMMAR ASKS ABOUT, one bit each, from Unicode 17
 * (docs/specs/dialect.md). A scalar may have several: every ASCII
 * punctuation character is punctuation, and those whose category is S are
 * symbols too. */
enum {
    /* Zs, and tab, LF, form feed and CR: CommonMark's Unicode whitespace,
     * which decides delimiter flanking and nothing else. */
    MARKDOWN_CORE_UNICODE_WHITESPACE = 1 << 0,
    /* P, and every ASCII punctuation character. */
    MARKDOWN_CORE_UNICODE_PUNCTUATION = 1 << 1,
    MARKDOWN_CORE_UNICODE_SYMBOL = 1 << 2,
    MARKDOWN_CORE_UNICODE_LETTER = 1 << 3,
    MARKDOWN_CORE_UNICODE_NUMBER = 1 << 4,
    /* A Unicode punctuation character as CommonMark 0.31 defines it: P or S. */
    MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL = MARKDOWN_CORE_UNICODE_PUNCTUATION | MARKDOWN_CORE_UNICODE_SYMBOL
};

/* The staged class table unicode_categories.inc defines, generated from
 * Unicode 17 by scripts/tooling/generate-unicode-categories.mjs: `pages` holds
 * the offset in `blocks` of each run of 256 scalars' classes. Hidden: every
 * lookup is inline in the library's own code, which addresses the table
 * PC-relative instead of through the global offset table. */
MARKDOWN_CORE_ATTRIBUTE((visibility("hidden"))) extern const uint16_t markdown_core_unicode_pages[];
MARKDOWN_CORE_ATTRIBUTE((visibility("hidden"))) extern const uint8_t markdown_core_unicode_blocks[];

/* The classes of a scalar, 0 for none and for every value that is not a
 * scalar, MARKDOWN_CORE_NO_SCALAR and the rest of int32_t included.
 *
 * Every scalar's classes are blocks[pages[s >> 8] + (s & 255)]. The first
 * page, U+0000..U+00FF, is the one whose offset is known without reading it:
 * the generator lays its block first, at offset 0, and asserts so. ASCII and
 * Latin-1 are therefore one load, and every other scalar two dependent loads
 * whatever its script. Inlined after the decoder, only its one-byte branch
 * decides the page test; after a 2-, 3- or 4-byte character the lookup
 * compares the scalar with 0xFF, and after a 4-byte one also with 0x110000.
 *
 * A caller tests the bits it asks about, and one asking two questions of a
 * scalar classifies it once. */
static inline uint8_t markdown_core_utf8proc_classes(int32_t uc) {
    const uint32_t scalar = (uint32_t)uc;
    if (scalar < 0x100) {
        return markdown_core_unicode_blocks[scalar];
    }
    if (scalar >= 0x110000) {
        return 0;
    }
    return markdown_core_unicode_blocks[(size_t)markdown_core_unicode_pages[scalar >> 8] + (scalar & 255)];
}

/* THE WIDTH OF AN ALPHANUMERIC CHARACTER at `str`, or 0 when the character
 * there is not one or the range is empty. "Alphanumeric" is what Pandoc's
 * grammar means by it, a Unicode letter or number: the class that ends a name
 * which has no delimiter after it, such as a bare citation key, where
 * `@张三，` must key `张三` and not the clause. */
static inline int markdown_core_utf8proc_alnum_width(const uint8_t *str, bufsize_t len) {
    int32_t scalar;
    if (len <= 0) {
        return 0;
    }
    const int width = markdown_core_utf8proc_decode(str, len, &scalar);
    return markdown_core_utf8proc_classes(scalar) & (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_NUMBER)
               ? width
               : 0;
}

/* Append the anchors module's Unicode projection of a valid UTF-8 literal. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len);

#ifdef __cplusplus
}
#endif

#endif
