#ifndef MARKDOWN_CORE_WRITE_H
#define MARKDOWN_CORE_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/markdown_core.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE WRITE INTERFACE (core-element-decoupling.md 9.9): how a value is
 * written in a slot of the context (markdown_core_slot_spell), the reverse
 * of what an element reads. The context finds the slot in the document and
 * holds no grammar: the element that writes the slot's kind spells the value
 * in its field's syntax, every element of the dialect may then refuse a
 * spelling that would change a construct of its own on the line, and the
 * element of each construct an earlier line leaves open says what it leaves
 * open at the line's start. Spelling reads a context, not a parse, so these
 * operations take no instance: what one element asks of another it asks
 * through a writing service (markdown_core_write_service). */

/* How the bytes of a spelled value stand in its field, for the elements
 * whose literal tokens own every byte they span (a code span, a formula, an
 * HTML tag, a comment): FREE, no token is asked; RAW, the value's bytes are
 * written as they are; ESCAPED, the value's bytes are written escaped, so
 * only a closer no escape keeps from closing counts. */
typedef enum markdown_core_tokens {
    MARKDOWN_CORE_TOKENS_FREE = 0,
    MARKDOWN_CORE_TOKENS_RAW,
    MARKDOWN_CORE_TOKENS_ESCAPED
} markdown_core_tokens;

/* What the constructs earlier lines leave open hold at the start of the
 * cursor's line: a bracket that a `]` on the line ends, and the quote of a
 * quoted string, or 0. */
typedef struct markdown_core_carried {
    bool bracket;
    uint8_t quote;
} markdown_core_carried;

/* A SITE: the slot a value is written in, on its line. Offsets are into
 * `line`, the cursor's line, its terminator not included: where the field
 * starts, or SIZE_MAX when it began on an earlier line; where the slot starts
 * and ends, and the cursor; where its construct's opener is, or SIZE_MAX.
 * `wrap` is the delimiter before the slot that its source opens with and
 * does not hold (markdown_core_write_ops, `wrap`), or 0; `row` whether the
 * line is a table's row, whose cells an unescaped `|` splits. */
typedef struct markdown_core_site {
    const uint8_t *line;
    size_t size;
    markdown_core_slot_kind kind;
    markdown_core_node_kind owner;
    bool closed, row;
    size_t field, start, cursor, end, opener;
    uint8_t wrap;
    markdown_core_carried carried;
} markdown_core_site;

/* A SPELLING: the text being written, in a buffer the value's size bounds
 * (eight bytes for each of its bytes and 64 more), the closer that finishes
 * a construct the document leaves open, and how the value's bytes stand in
 * it. */
typedef struct markdown_core_spelling {
    uint8_t *text;
    size_t size;
    const char *closer;
    markdown_core_tokens tokens;
} markdown_core_spelling;

/* Where a slot's field and its source lie, for `wrap`: the source, the
 * start of the cursor's line, of the field and of the slot, and its prefix's
 * start when `prefixed`. */
typedef struct markdown_core_wrap_site {
    const uint8_t *source;
    size_t line, field, start, prefix;
    bool prefixed;
} markdown_core_wrap_site;

/* A PROPERTIES ENVELOPE AT THE CURSOR (markdown_core_facet, `envelope`):
 * where the envelope's opening line's scope ends, and the slot the cursor
 * is in, when `slot`: its kind, from `from` to `to`, its field starting at
 * `field`, with `wrap` and `closed` as a site's. */
typedef struct markdown_core_envelope {
    size_t opening;
    bool slot;
    markdown_core_slot_kind kind;
    size_t from, to, field;
    uint8_t wrap;
    bool closed;
} markdown_core_envelope;

/* WHAT AN ELEMENT WRITES (markdown_core_element, `writes`). Each entry may
 * be NULL, for what the element does not take part in.
 *
 * `slots`: the bits of the slot kinds it writes; `owners`, the kinds it
 * writes them for, ended by MARKDOWN_CORE_KIND_NONE, or NULL for any. The
 * writer of a slot is the element that names its owner, else the one that
 * names no owners (markdown_core_writer).
 * `alone`: its spelling answers for the constructs around it, so no other
 * element's `keeps` is asked.
 * `prefix`: the slot kind whose field, before it in its construct, the
 * slot's text depends on, or NONE.
 * `wrap`: the delimiter before the field its source opens with and does not
 * hold, or 0.
 * `spell`: writes `value` in the slot's syntax into `out`, its closer and
 * tokens with it; `*held` false when the slot cannot hold it. Not OK only
 * when storage could not be had.
 * `keeps`: `*kept` whether the spelling leaves the element's constructs on
 * the line as they are. Not OK only when storage could not be had.
 * `carry`: what the construct of kind `kind` the element began from `start`
 * to `end` on an earlier line, still open, leaves open at `line`, the start
 * of the cursor's line. */
typedef struct markdown_core_write_ops {
    uint32_t slots;
    const markdown_core_node_kind *owners;
    bool alone;
    markdown_core_slot_kind (*prefix)(markdown_core_slot_kind slot);
    uint8_t (*wrap)(markdown_core_slot_kind slot, markdown_core_node_kind owner, const markdown_core_wrap_site *at);
    markdown_core_status (*spell)(const markdown_core_site *site, const uint8_t *value, size_t size,
                                  markdown_core_spelling *out, bool *held);
    markdown_core_status (*keeps)(const markdown_core_site *site, const uint8_t *value, size_t size,
                                  const markdown_core_spelling *spelled, bool *kept);
    void (*carry)(markdown_core_node_kind kind, const uint8_t *source, size_t start, size_t end, size_t line,
                  markdown_core_carried *carried);
} markdown_core_write_ops;

#define MARKDOWN_CORE_SLOT_BIT(slot) ((uint32_t)1 << (slot))

/* WRITING HELPERS, which hold no grammar. */

static inline void markdown_core_write_put(markdown_core_spelling *out, uint8_t c) { out->text[out->size++] = c; }

static inline bool markdown_core_write_among(uint8_t c, const char *set) { return c && strchr(set, c); }

/* Writes `value` as it is, false when it holds a byte of `barred`, or a
 * `|` on a table's row, which would split its cell. */
static inline bool markdown_core_write_plain(markdown_core_spelling *out, const uint8_t *value, size_t size,
                                             const char *barred, bool row) {
    for (size_t i = 0; i < size; i++) {
        if (markdown_core_write_among(value[i], barred) || (row && value[i] == '|')) {
            return false;
        }
        markdown_core_write_put(out, value[i]);
    }
    return true;
}

/* Whether every byte of `value` is an ASCII letter, a numeral or one of
 * `also`, and there is one. */
static inline bool markdown_core_write_word(const uint8_t *value, size_t size, const char *also) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t c = value[i];
        if (!((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
              markdown_core_write_among(c, also))) {
            return false;
        }
    }
    return size > 0;
}

/* Whether `value` is spaces and tabs, or nothing. */
static inline bool markdown_core_write_blank(const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (value[i] != ' ' && value[i] != '\t') {
            return false;
        }
    }
    return true;
}

/* Whether the line before the slot leaves an `open` that no `close`
 * matches, a backslash keeping the byte after it from either, and with a
 * bracket an earlier line leaves open counted for `[`. */
static inline bool markdown_core_write_opened(const markdown_core_site *site, uint8_t open, uint8_t close) {
    size_t depth = open == '[' && site->carried.bracket;
    for (size_t i = 0; i < site->start && i < site->size; i++) {
        const uint8_t c = site->line[i];
        if (c == '\\') {
            i++;
        } else if (c == open) {
            depth++;
        } else if (c == close && depth) {
            depth--;
        }
    }
    return depth > 0;
}

/* The bytes of the line after the slot that a field written there may read
 * on into: to the line's end, or to the `]` that closes a bracket the slot
 * stands in. */
static inline size_t markdown_core_write_rest(const markdown_core_site *site) {
    const uint8_t *tail = site->line + site->end;
    size_t rest = site->size - site->end;
    if (markdown_core_write_opened(site, '[', ']')) {
        size_t depth = 0, i = 0;
        for (; i < rest && (tail[i] != ']' || depth); i++) {
            if (tail[i] == '\\') {
                i++;
            } else {
                depth += tail[i] == '[';
                depth -= tail[i] == ']';
            }
        }
        rest = i < rest ? i : rest;
    }
    return rest;
}

/* The class of a byte to a delimiter run beside it: a space or none, ASCII
 * punctuation, or neither. */
static inline int markdown_core_write_class(uint8_t c) {
    return c == ' ' || c == '\t' || !c ? 0 : markdown_core_write_among(c, "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~") ? 1 : 2;
}

/* Whether a delimiter run right after the slot keeps the class of the byte
 * before it: what it had, the slot's last byte, and what it gets, the
 * spelling's last byte or, for an empty one, the byte before the slot. */
static inline bool markdown_core_write_flanked(const markdown_core_site *site, const markdown_core_spelling *spelled) {
    const uint8_t was = site->end > 0 ? site->line[site->end - 1] : 0;
    const uint8_t now = spelled->size     ? spelled->text[spelled->size - 1]
                        : site->start > 0 ? site->line[site->start - 1]
                                          : 0;
    return markdown_core_write_class(was) == markdown_core_write_class(now);
}

/* Whether a delimiter run `run` right after the slot, when no closer
 * stands between, keeps its flank (markdown_core_write_flanked). */
static inline bool markdown_core_write_runs(const markdown_core_site *site, const markdown_core_spelling *spelled,
                                            const char *run) {
    const size_t width = strlen(run);
    const bool after = !(spelled->closer && *spelled->closer) && site->size - site->end >= width &&
                       !memcmp(site->line + site->end, run, width);
    return !after || markdown_core_write_flanked(site, spelled);
}

/* Whether the value holds a literal token's `token`, which owns every byte
 * it spans: a field that keeps its bytes, or one whose escapes no closer
 * heeds, cannot hold it (markdown_core_tokens). */
static inline bool markdown_core_write_holds(const uint8_t *value, size_t size, const char *token) {
    const size_t width = strlen(token);
    for (size_t i = 0; i + width <= size; i++) {
        if (!memcmp(value + i, token, width)) {
            return true;
        }
    }
    return false;
}

/* Whether a reading of the field that `end` does, from the spelling with
 * the line's bytes after the slot it may read on into (markdown_core_write_rest)
 * after it, and the byte before the slot before it (0 at the line's start),
 * reads the spelling whole: `end` answers where it stops, -1 for only at
 * the field's closer. Not OK only when storage could not be had. */
markdown_core_status
markdown_core_write_reads(const markdown_core_site *site, const markdown_core_spelling *spelled,
                          bufsize_t (*end)(uint8_t before, const unsigned char *data, bufsize_t length), bool *whole);

/* THE WRITING SERVICES: what an element offers the others' spellings, under
 * a name of elements/services.h, found among the descriptors of the dialect
 * the context reads, since spelling holds no instance; the last element
 * that provides the name answers, or none. */
const void *markdown_core_write_service(const char *name);
/* What the element at `index` of the dialect writes, or NULL; `*more`
 * false past the last. */
const markdown_core_write_ops *markdown_core_writes(size_t index, bool *more);
/* The element of the dialect that writes slots of kind `slot` of a
 * construct of kind `owner`, or NULL for none. */
const markdown_core_write_ops *markdown_core_writer(markdown_core_slot_kind slot, markdown_core_node_kind owner);

#ifdef __cplusplus
}
#endif

#endif
