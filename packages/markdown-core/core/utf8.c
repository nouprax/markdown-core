#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "markdown_core_ctype.h"
#include "utf8.h"

static const int8_t utf8proc_utf8class[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0};

static void encode_unknown(markdown_core_strbuf *buf) {
    static const uint8_t repl[] = {239, 191, 189};
    markdown_core_strbuf_put(buf, repl, 3);
}

static int utf8proc_charlen(const uint8_t *str, bufsize_t str_len) {
    int length, i;

    if (!str_len) {
        return 0;
    }

    length = utf8proc_utf8class[str[0]];

    if (!length) {
        return -1;
    }

    if (str_len >= 0 && (bufsize_t)length > str_len) {
        return -str_len;
    }

    for (i = 1; i < length; i++) {
        if ((str[i] & 0xC0) != 0x80) {
            return -i;
        }
    }

    return length;
}

int markdown_core_utf8proc_iterate_general(const uint8_t *str, bufsize_t str_len, int32_t *dst) {
    int length;
    int32_t uc = -1;

    *dst = -1;
    length = utf8proc_charlen(str, str_len);
    if (length < 0) {
        return -1;
    }

    switch (length) {
    case 1:
        uc = str[0];
        break;
    case 2:
        uc = ((str[0] & 0x1F) << 6) + (str[1] & 0x3F);
        if (uc < 0x80) {
            uc = -1;
        }
        break;
    case 3:
        uc = ((str[0] & 0x0F) << 12) + ((str[1] & 0x3F) << 6) + (str[2] & 0x3F);
        if (uc < 0x800 || (uc >= 0xD800 && uc < 0xE000)) {
            uc = -1;
        }
        break;
    case 4:
        uc = ((str[0] & 0x07) << 18) + ((str[1] & 0x3F) << 12) + ((str[2] & 0x3F) << 6) + (str[3] & 0x3F);
        if (uc < 0x10000 || uc >= 0x110000) {
            uc = -1;
        }
        break;
    }

    if (uc < 0) {
        return -1;
    }

    *dst = uc;
    return length;
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

/* The fold table's entry for `c`, or NULL when it folds to itself. */
static const uint32_t *S_case_fold_entry(int32_t c) {
    size_t low = 0, high = CF_TABLE_SIZE;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int32_t code = (int32_t)CF_CODE_POINT(cf_table[mid]);
        if (c < code) {
            high = mid;
        } else if (c > code) {
            low = mid + 1;
        } else {
            return &cf_table[mid];
        }
    }
    return NULL;
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
                memcpy(out, cf_repl + CF_REPL_IDX(*entry), CF_REPL_SIZE(*entry));
                out += CF_REPL_SIZE(*entry);
            } else {
                memcpy(out, str, (size_t)char_len);
                out += char_len;
            }
        }
        str += char_len;
        len -= char_len;
    }
    S_finish_image(dest, out);
}

// matches anything in the P[cdefios] classes.
#include "punctuation_or_symbol.inc"

int markdown_core_utf8proc_is_punctuation(int32_t uc) {
    return ((uc < 128 && markdown_core_ispunct((char)uc)) || uc == 161 || uc == 167 || uc == 171 || uc == 182 ||
            uc == 183 || uc == 187 || uc == 191 || uc == 894 || uc == 903 || (uc >= 1370 && uc <= 1375) || uc == 1417 ||
            uc == 1418 || uc == 1470 || uc == 1472 || uc == 1475 || uc == 1478 || uc == 1523 || uc == 1524 ||
            uc == 1545 || uc == 1546 || uc == 1548 || uc == 1549 || uc == 1563 || uc == 1566 || uc == 1567 ||
            (uc >= 1642 && uc <= 1645) || uc == 1748 || (uc >= 1792 && uc <= 1805) || (uc >= 2039 && uc <= 2041) ||
            (uc >= 2096 && uc <= 2110) || uc == 2142 || uc == 2404 || uc == 2405 || uc == 2416 || uc == 2800 ||
            uc == 3572 || uc == 3663 || uc == 3674 || uc == 3675 || (uc >= 3844 && uc <= 3858) || uc == 3860 ||
            (uc >= 3898 && uc <= 3901) || uc == 3973 || (uc >= 4048 && uc <= 4052) || uc == 4057 || uc == 4058 ||
            (uc >= 4170 && uc <= 4175) || uc == 4347 || (uc >= 4960 && uc <= 4968) || uc == 5120 || uc == 5741 ||
            uc == 5742 || uc == 5787 || uc == 5788 || (uc >= 5867 && uc <= 5869) || uc == 5941 || uc == 5942 ||
            (uc >= 6100 && uc <= 6102) || (uc >= 6104 && uc <= 6106) || (uc >= 6144 && uc <= 6154) || uc == 6468 ||
            uc == 6469 || uc == 6686 || uc == 6687 || (uc >= 6816 && uc <= 6822) || (uc >= 6824 && uc <= 6829) ||
            (uc >= 7002 && uc <= 7008) || (uc >= 7164 && uc <= 7167) || (uc >= 7227 && uc <= 7231) || uc == 7294 ||
            uc == 7295 || (uc >= 7360 && uc <= 7367) || uc == 7379 || (uc >= 8208 && uc <= 8231) ||
            (uc >= 8240 && uc <= 8259) || (uc >= 8261 && uc <= 8273) || (uc >= 8275 && uc <= 8286) || uc == 8317 ||
            uc == 8318 || uc == 8333 || uc == 8334 || (uc >= 8968 && uc <= 8971) || uc == 9001 || uc == 9002 ||
            (uc >= 10088 && uc <= 10101) || uc == 10181 || uc == 10182 || (uc >= 10214 && uc <= 10223) ||
            (uc >= 10627 && uc <= 10648) || (uc >= 10712 && uc <= 10715) || uc == 10748 || uc == 10749 ||
            (uc >= 11513 && uc <= 11516) || uc == 11518 || uc == 11519 || uc == 11632 || (uc >= 11776 && uc <= 11822) ||
            (uc >= 11824 && uc <= 11842) || (uc >= 12289 && uc <= 12291) || (uc >= 12296 && uc <= 12305) ||
            (uc >= 12308 && uc <= 12319) || uc == 12336 || uc == 12349 || uc == 12448 || uc == 12539 || uc == 42238 ||
            uc == 42239 || (uc >= 42509 && uc <= 42511) || uc == 42611 || uc == 42622 || (uc >= 42738 && uc <= 42743) ||
            (uc >= 43124 && uc <= 43127) || uc == 43214 || uc == 43215 || (uc >= 43256 && uc <= 43258) || uc == 43310 ||
            uc == 43311 || uc == 43359 || (uc >= 43457 && uc <= 43469) || uc == 43486 || uc == 43487 ||
            (uc >= 43612 && uc <= 43615) || uc == 43742 || uc == 43743 || uc == 43760 || uc == 43761 || uc == 44011 ||
            uc == 64830 || uc == 64831 || (uc >= 65040 && uc <= 65049) || (uc >= 65072 && uc <= 65106) ||
            (uc >= 65108 && uc <= 65121) || uc == 65123 || uc == 65128 || uc == 65130 || uc == 65131 ||
            (uc >= 65281 && uc <= 65283) || (uc >= 65285 && uc <= 65290) || (uc >= 65292 && uc <= 65295) ||
            uc == 65306 || uc == 65307 || uc == 65311 || uc == 65312 || (uc >= 65339 && uc <= 65341) || uc == 65343 ||
            uc == 65371 || uc == 65373 || (uc >= 65375 && uc <= 65381) || (uc >= 65792 && uc <= 65794) || uc == 66463 ||
            uc == 66512 || uc == 66927 || uc == 67671 || uc == 67871 || uc == 67903 || (uc >= 68176 && uc <= 68184) ||
            uc == 68223 || (uc >= 68336 && uc <= 68342) || (uc >= 68409 && uc <= 68415) ||
            (uc >= 68505 && uc <= 68508) || (uc >= 69703 && uc <= 69709) || uc == 69819 || uc == 69820 ||
            (uc >= 69822 && uc <= 69825) || (uc >= 69952 && uc <= 69955) || uc == 70004 || uc == 70005 ||
            (uc >= 70085 && uc <= 70088) || uc == 70093 || (uc >= 70200 && uc <= 70205) || uc == 70854 ||
            (uc >= 71105 && uc <= 71113) || (uc >= 71233 && uc <= 71235) || (uc >= 74864 && uc <= 74868) ||
            uc == 92782 || uc == 92783 || uc == 92917 || (uc >= 92983 && uc <= 92987) || uc == 92996 || uc == 113823);
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
