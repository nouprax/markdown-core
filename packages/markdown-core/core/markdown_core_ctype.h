#ifndef MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H
#define MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

#include "config.h"

/** Locale-independent versions of functions from ctype.h.
 * We want markdown_core to behave the same no matter what the system locale.
 *
 * Each predicate is one load from the shared class table, so it is defined
 * here, where every caller -- most of them walking a buffer byte by byte --
 * can see it, rather than behind a call into another translation unit that
 * costs more to reach than to evaluate. None of them is part of the exported
 * surface.
 */

/* 1 = space, 2 = punct, 3 = digit, 4 = alpha, 0 = other. */
extern const uint8_t markdown_core_ctype_class[256];

/* A "whitespace" character as the spec defines it: space, tab, LF, CR. */
static inline int markdown_core_isspace(char c) { return markdown_core_ctype_class[(uint8_t)c] == 1; }

/* An ASCII punctuation character. */
static inline int markdown_core_ispunct(char c) { return markdown_core_ctype_class[(uint8_t)c] == 2; }

static inline int markdown_core_isalnum(char c) {
    uint8_t result = markdown_core_ctype_class[(uint8_t)c];
    return result == 3 || result == 4;
}

static inline int markdown_core_isdigit(char c) { return markdown_core_ctype_class[(uint8_t)c] == 3; }

static inline int markdown_core_isalpha(char c) { return markdown_core_ctype_class[(uint8_t)c] == 4; }

#ifdef __cplusplus
}
#endif

/* Source-line boundaries use ASCII CR/LF in every syntax scanner. */
static inline int markdown_core_is_line_end(unsigned char c) { return c == '\n' || c == '\r'; }

/* THE FIRST OF THREE BYTES in [cursor, end), or `end` when none occurs.
 *
 * Scanners that stop only at a few ASCII bytes -- the physical span alphabet
 * NUL, CR and LF; a link label's `[`, `]` and `\` -- ask this rather than
 * testing each byte; `cursor` must not be past `end`. A bounded memcpy
 * probes a whole word without alignment or aliasing assumptions and never
 * reads past `end`. The unsigned zero-byte test on the word XOR each target
 * answers only whether one occurs, so it is endian-independent; bytes then
 * resolve the first occurrence and the tail.
 * Each word that holds none consumes eight bytes, and an occurrence costs at
 * most one extra word probe before byte resolution, so work stays linear
 * even for input made entirely of the targets.
 *
 * Nothing here depends on what the other bytes are. In UTF-8 every byte of a
 * multi-byte character is at or above 0x80, so no part of one is ever an
 * ASCII target, and a word of any script is skipped at the same cost. Inlining
 * is explicit so the three targets fold into constants at every call. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) const
    unsigned char *markdown_core_find_byte3(const unsigned char *cursor, const unsigned char *end, unsigned char a,
                                            unsigned char b, unsigned char c) {
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t highs = UINT64_C(0x8080808080808080);
    while ((size_t)(end - cursor) >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, cursor, sizeof(word));
        uint64_t xa = word ^ (ones * a), xb = word ^ (ones * b), xc = word ^ (ones * c);
        if ((((xa - ones) & ~xa) | ((xb - ones) & ~xb) | ((xc - ones) & ~xc)) & highs) {
            break;
        }
        cursor += sizeof(word);
    }
    while (cursor < end && *cursor != a && *cursor != b && *cursor != c) {
        cursor++;
    }
    return cursor;
}

#endif
