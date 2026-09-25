#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "markdown_core_ctype.h"
#include "utf8.h"

static void encode_unknown(markdown_core_strbuf *buf) {
    static const uint8_t repl[] = {239, 191, 189};
    markdown_core_strbuf_put(buf, repl, 3);
}

/* The length of a scalar's UTF-8 encoding, for a scalar below 0x110000. */
static inline int S_scalar_width(int32_t uc) { return uc < 0x80 ? 1 : uc < 0x800 ? 2 : uc < 0x10000 ? 3 : 4; }

/* The UTF-8 encoding of a scalar below 0x110000, `width` bytes long (see
 * S_scalar_width), written at `dst`. The one encoder: appending a character
 * and writing a projected literal into reserved storage both go through it. */
static inline void S_encode_scalar(int32_t uc, int width, uint8_t *dst) {
    switch (width) {
    case 1:
        dst[0] = (uint8_t)(uc);
        return;
    case 2:
        dst[0] = (uint8_t)(0xC0 + (uc >> 6));
        dst[1] = 0x80 + (uc & 0x3F);
        return;
    case 3:
        dst[0] = (uint8_t)(0xE0 + (uc >> 12));
        dst[1] = 0x80 + ((uc >> 6) & 0x3F);
        dst[2] = 0x80 + (uc & 0x3F);
        return;
    default:
        dst[0] = (uint8_t)(0xF0 + (uc >> 18));
        dst[1] = 0x80 + ((uc >> 12) & 0x3F);
        dst[2] = 0x80 + ((uc >> 6) & 0x3F);
        dst[3] = 0x80 + (uc & 0x3F);
        return;
    }
}

void markdown_core_utf8proc_encode_char(int32_t uc, markdown_core_strbuf *buf) {
    uint8_t dst[4];

    assert(uc >= 0);

    if (uc >= 0x110000) {
        encode_unknown(buf);
        return;
    }
    int width = S_scalar_width(uc);
    S_encode_scalar(uc, width, dst);
    markdown_core_strbuf_put(buf, dst, width);
}

/* A WRITE CURSOR OVER RESERVED STORAGE, for the projections below that turn a
 * literal into its image one character at a time. Each writes straight into
 * `buf` through a local cursor `out` and limit `end` instead of appending
 * character by character, and checks before every character that the room
 * left holds that character's image, whose exact length it knows. The room
 * ends where the allocation does, less the terminator's byte, which is never
 * past MARKDOWN_CORE_STRBUF_LIMIT: growth caps the allocation at the limit
 * and its terminator (buffer.c), so the content cannot follow the allocation
 * past the limit. When the room is short, the cursor is handed here, which
 * poisons the buffer when the character's image would take the content past
 * the limit -- the byte where appending it would have been refused -- and
 * otherwise reserves for the rest of the literal at once, an upper bound the
 * caller states, clamped to the limit because the bound can pass it while the
 * image itself fits. The cursor stays a local of its caller: bytes written
 * through it cannot alias it, so it lives in a register. */

/* The image written so far becomes the buffer's content. A buffer that never
 * reserved has had nothing written into it. */
static inline void S_finish_image(markdown_core_strbuf *buf, uint8_t *out) {
    if (markdown_core_strbuf_owns(buf)) {
        buf->size = (bufsize_t)(out - buf->ptr);
        buf->ptr[buf->size] = '\0';
    }
}

/* The end of the room: one byte of the allocation is kept for the
 * terminator, and the allocation never reaches past the limit. */
static inline uint8_t *S_image_end(const markdown_core_strbuf *buf) { return buf->ptr + buf->asize - 1; }

/* Returns the cursor in the grown buffer, whose room ends at S_image_end, or
 * NULL when the buffer is poisoned. What was written before stays the
 * buffer's terminated content either way. */
static uint8_t *S_reserve_image(markdown_core_strbuf *buf, uint8_t *out, size_t need, size_t bound) {
    S_finish_image(buf, out);
    size_t room = (size_t)(MARKDOWN_CORE_STRBUF_LIMIT - buf->size);
    if (need > room) {
        buf->oom = 1;
        return NULL;
    }
    markdown_core_strbuf_grow(buf, buf->size + (bufsize_t)(bound < room ? bound : room));
    return buf->oom ? NULL : buf->ptr + buf->size;
}

#include "case_fold.inc"
#include "case_fold_index.inc"

/* The fold table's entry for `c`, below CF_MAX, or NULL when it folds to
 * itself. Two dependent loads through the generated index, for every
 * character whatever its script: it used to be a binary search of the
 * 1,559-entry table, about eleven probes that ASCII never paid and every
 * other character did (#405). */
static inline const uint32_t *S_case_fold_entry(int32_t c) {
    uint16_t number = cf_leaves[cf_pages[c >> CF_LEAF_BITS]][c & ((1 << CF_LEAF_BITS) - 1)];
    return number ? &cf_table[number - 1] : NULL;
}

/* One to seven bytes -- a character of two or more, or its fold -- in two
 * fixed-size moves that may overlap, reading and writing only those bytes. A
 * memcpy whose length is the character's was a library call per character. */
static inline void S_copy_image(uint8_t *out, const uint8_t *in, size_t n) {
    if (n >= 4) {
        uint32_t head, tail;
        memcpy(&head, in, 4);
        memcpy(&tail, in + n - 4, 4);
        memcpy(out, &head, 4);
        memcpy(out + n - 4, &tail, 4);
    } else if (n >= 2) {
        uint16_t head, tail;
        memcpy(&head, in, 2);
        memcpy(&tail, in + n - 2, 2);
        memcpy(out, &head, 2);
        memcpy(out + n - 2, &tail, 2);
    } else {
        *out = *in;
    }
}

/* THE REFERENCE-LABEL NORMAL FORM, in one pass: case fold, then drop leading
 * and trailing whitespace and collapse each interior run to one space.
 *
 * It was three passes -- fold one character at a time into the buffer, trim
 * both ends, collapse the runs -- and they compose character by character:
 * folding neither creates nor changes a whitespace byte (a fold's image is
 * letters and marks, and a byte that is not a character is copied as it is),
 * so a run seen in the input is the run the later passes saw. A run is
 * therefore remembered and written as one space only when a character follows
 * it and one precedes it, which is the trim.
 *
 * The output goes through the write cursor above: a character's image is a
 * pending space and the character folded, lowered or copied. No fold is longer
 * than three times its character (the table's widest is U+0390, two bytes to
 * six), every other character is copied and a run shrinks, so three times the
 * rest of the label bounds what remains. */
void markdown_core_utf8proc_normalize_label(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len) {
    uint8_t *out = dest->ptr + dest->size, *end = out;
    const bufsize_t first = dest->size;
    bool space = false;
    while (len > 0) {
        /* Total, so the walk always moves forward. The U+FFFD substitution
         * this used to make for a byte that starts no character was the one
         * place the library REPAIRED malformed input, which markdown_core.h
         * says it does not do. */
        int32_t c;
        bufsize_t char_len = markdown_core_utf8proc_step(str, len, &c);
        if (char_len == 1 && markdown_core_isspace((char)str[0])) {
            space = out - dest->ptr > first;
        } else {
            const uint32_t *entry = char_len > 1 && c < CF_MAX ? S_case_fold_entry(c) : NULL;
            /* Seven bytes hold any character's image; only short of that is
             * this one's exact length asked for. */
            if (end - out < 7) {
                size_t width = (size_t)space + (entry ? CF_REPL_SIZE(*entry) : (size_t)char_len);
                if ((size_t)(end - out) < width) {
                    if (!(out = S_reserve_image(dest, out, width, width + 3 * (size_t)(len - char_len)))) {
                        return;
                    }
                    end = S_image_end(dest);
                }
            }
            if (space) {
                *out++ = ' ';
                space = false;
            }
            if (char_len == 1) {
                *out++ = str[0] >= 'A' && str[0] <= 'Z' ? (uint8_t)(str[0] + ('a' - 'A')) : str[0];
            } else if (entry) {
                S_copy_image(out, cf_repl + CF_REPL_IDX(*entry), CF_REPL_SIZE(*entry));
                out += CF_REPL_SIZE(*entry);
            } else {
                S_copy_image(out, str, (size_t)char_len);
                out += char_len;
            }
        }
        str += char_len;
        len -= char_len;
    }
    S_finish_image(dest, out);
}

#include "unicode_categories.inc"

#include "anchor_scalars.inc"

/* The anchors module's image of one scalar, or 0 when the scalar is removed.
 * Three dependent loads for every scalar: the generated table is staged by
 * page, leaf and class, so no script is searched for and none is special. */
static inline int32_t anchor_scalar(int32_t uc) {
    if ((uint32_t)uc >= 0x110000) {
        return 0;
    }
    uint8_t page = anchor_pages[uc >> (ANCHOR_LEAF_BITS + ANCHOR_PAGE_BITS)];
    uint16_t leaf = anchor_leaves[page][(uc >> ANCHOR_LEAF_BITS) & ((1 << ANCHOR_PAGE_BITS) - 1)];
    uint8_t projected = anchor_classes[leaf][uc & ((1 << ANCHOR_LEAF_BITS) - 1)];
    return projected ? uc + anchor_deltas[projected] : 0;
}

/* Consume a complete literal here so UTF-8 decoding, scalar projection and
 * encoding share one loop and write through the cursor above.
 *
 * The bound it reserves for is twice the rest of the literal. The generator
 * asserts that no scalar's image is longer than 3/2 of its own encoding, and a
 * byte that begins no character is stepped over as a one-byte scalar whose
 * image is at most two bytes. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len) {
    uint8_t *out = dest->ptr + dest->size, *end = out;
    for (bufsize_t at = 0; at < len;) {
        int32_t scalar;
        at += markdown_core_utf8proc_step(str + at, len - at, &scalar);
        scalar = anchor_scalar(scalar);
        if (!scalar) {
            continue;
        }
        int width = S_scalar_width(scalar);
        /* Four bytes hold any scalar's image; only short of that is this
         * one's exact width compared. */
        if (end - out < 4) {
            if (end - out < width) {
                if (!(out = S_reserve_image(dest, out, (size_t)width, (size_t)width + 2 * (size_t)(len - at)))) {
                    return;
                }
                end = S_image_end(dest);
            }
        }
        S_encode_scalar(scalar, width, out);
        out += width;
    }
    S_finish_image(dest, out);
}
