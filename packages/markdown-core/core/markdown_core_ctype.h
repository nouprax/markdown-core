#ifndef MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H
#define MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

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

#endif
