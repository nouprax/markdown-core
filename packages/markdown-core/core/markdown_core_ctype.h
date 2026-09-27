#ifndef MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H
#define MARKDOWN_CORE_MARKDOWN_CORE_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "config.h"
#include "markdown-core.h"

/** Locale-independent versions of functions from ctype.h.
 * We want markdown_core to behave the same no matter what the system locale.
 *
 * Each predicate is one load from the shared class table or a comparison or
 * two, so it is defined here, where every caller -- most of them walking a
 * buffer byte by byte -- can see it, rather than behind a call into another
 * translation unit that costs more to reach than to evaluate. None of them is
 * part of the exported surface.
 */

/* 1 = punct, 2 = digit, 3 = alpha, 0 = other. */
extern const uint8_t markdown_core_ctype_class[256];

/* An ASCII punctuation character. */
static inline int markdown_core_ispunct(char c) { return markdown_core_ctype_class[(uint8_t)c] == 1; }

static inline int markdown_core_isalnum(char c) {
    uint8_t result = markdown_core_ctype_class[(uint8_t)c];
    return result == 2 || result == 3;
}

static inline int markdown_core_isdigit(char c) { return markdown_core_ctype_class[(uint8_t)c] == 2; }

static inline int markdown_core_isalpha(char c) { return markdown_core_ctype_class[(uint8_t)c] == 3; }

/* WHITESPACE, in the classes docs/specs/dialect.md names. CommonMark 0.31.2
 * indents, separates and trims with spaces and tabs, and ends a line with
 * LF, CR or CR LF; the dialect's whitespace is those bytes together. No
 * other byte is whitespace here: not vertical tab, not form feed, and no
 * other control character. Unicode whitespace, a class of scalars that
 * decides delimiter flanking and nothing else, is in utf8.h. */

/* A space (U+0020) or a tab (U+0009). */
static inline int markdown_core_is_space_or_tab(unsigned char c) { return c == ' ' || c == '\t'; }

/* A byte of a line ending: LF, or CR alone or before LF. */
static inline int markdown_core_is_line_end(unsigned char c) { return c == '\n' || c == '\r'; }

/* Whitespace: a space, a tab or a byte of a line ending. */
static inline int markdown_core_is_whitespace(unsigned char c) {
    return markdown_core_is_space_or_tab(c) || markdown_core_is_line_end(c);
}

/* Whether only spaces and tabs come before the first line ending in
 * data[at, end), or before `end` when there is none: a blank rest of line. */
static inline int markdown_core_is_blank_to_line_end(const unsigned char *data, bufsize_t at, bufsize_t end) {
    while (at < end && markdown_core_is_space_or_tab(data[at])) {
        at++;
    }
    return at == end || markdown_core_is_line_end(data[at]);
}

/* Past the spaces, tabs and at most one line ending that begin data[at, end).
 * CommonMark separates a link's parts this way, and the dialect does wherever
 * one line break may fall between two parts of a construct. */
static inline bufsize_t markdown_core_skip_spaces_and_line_end(const unsigned char *data, bufsize_t at, bufsize_t end) {
    while (at < end && markdown_core_is_space_or_tab(data[at])) {
        at++;
    }
    bufsize_t after = at < end && data[at] == '\r' ? at + 1 : at;
    if (after < end && data[after] == '\n') {
        after++;
    }
    if (after == at) {
        return at;
    }
    at = after;
    while (at < end && markdown_core_is_space_or_tab(data[at])) {
        at++;
    }
    return at;
}

#ifdef __cplusplus
}
#endif

/* THE FIRST BYTE OF A CLASS A SCAN STOPS AT: the index of the first byte of
 * data[at, end) whose class meets `stops`, or `end` when none does; `at` must
 * not be past `end`.
 *
 * A scanner that runs until one of the bytes it tells apart names those bytes
 * once, as a table of 256 class masks, and says which classes stop it. A
 * table may give a byte several classes for several scans, as the attribute
 * grammar's does; a byte whose class meets `stops` ends this one.
 *
 * Each byte is decided by one table load and one test, however many bytes
 * stop the scan, and the scan returns at the first stop, so a short run
 * costs only its own bytes. Eight bytes are decided per bound check, so a
 * long run pays that check once per eight. In UTF-8 every byte of a
 * multi-byte character is at or above 0x80, so a table that names only ASCII
 * bytes skips text of any script at the same cost. Where the compiler inlines
 * it, the table and the stops fold into the call. Inlining is not forced: the
 * attribute grammar scans in four places, and forcing four unrolled copies
 * into it made it too large to inline into its own callers. */
static inline bufsize_t markdown_core_scan_to_class(const uint8_t classes[256], uint8_t stops,
                                                    const unsigned char *data, bufsize_t at, bufsize_t end) {
    for (; end - at >= 8; at += 8) {
        if (classes[data[at]] & stops) {
            return at;
        }
        if (classes[data[at + 1]] & stops) {
            return at + 1;
        }
        if (classes[data[at + 2]] & stops) {
            return at + 2;
        }
        if (classes[data[at + 3]] & stops) {
            return at + 3;
        }
        if (classes[data[at + 4]] & stops) {
            return at + 4;
        }
        if (classes[data[at + 5]] & stops) {
            return at + 5;
        }
        if (classes[data[at + 6]] & stops) {
            return at + 6;
        }
        if (classes[data[at + 7]] & stops) {
            return at + 7;
        }
    }
    while (at < end && !(classes[data[at]] & stops)) {
        at++;
    }
    return at;
}

#endif
