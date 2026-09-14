#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "diagnostics.h"
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

int markdown_core_utf8proc_iterate(const uint8_t *str, bufsize_t str_len, int32_t *dst) {
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

void markdown_core_utf8proc_encode_char(int32_t uc, markdown_core_strbuf *buf) {
    uint8_t dst[4];
    bufsize_t len = 0;

    assert(uc >= 0);

    if (uc < 0x80) {
        dst[0] = (uint8_t)(uc);
        len = 1;
    } else if (uc < 0x800) {
        dst[0] = (uint8_t)(0xC0 + (uc >> 6));
        dst[1] = 0x80 + (uc & 0x3F);
        len = 2;
    } else if (uc < 0x10000) {
        dst[0] = (uint8_t)(0xE0 + (uc >> 12));
        dst[1] = 0x80 + ((uc >> 6) & 0x3F);
        dst[2] = 0x80 + (uc & 0x3F);
        len = 3;
    } else if (uc < 0x110000) {
        dst[0] = (uint8_t)(0xF0 + (uc >> 18));
        dst[1] = 0x80 + ((uc >> 12) & 0x3F);
        dst[2] = 0x80 + ((uc >> 6) & 0x3F);
        dst[3] = 0x80 + (uc & 0x3F);
        len = 4;
    } else {
        encode_unknown(buf);
        return;
    }

    markdown_core_strbuf_put(buf, dst, len);
}

#include "case_fold.inc"

static int S_case_fold_compare(const void *left, const void *right) {
    uint32_t left_entry = *(const uint32_t *)left;
    uint32_t right_entry = *(const uint32_t *)right;

    return (int32_t)CF_CODE_POINT(left_entry) - (int32_t)CF_CODE_POINT(right_entry);
}

void markdown_core_utf8proc_case_fold(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len) {
    int32_t c;

    while (len > 0) {
        /* An ASCII run folds byte by byte into room reserved once: only a
         * capital letter changes, and no byte needs decoding to know it is
         * one. Decoding resumes at the first byte above ASCII. */
        bufsize_t run = 0;
        while (run < len && str[run] < 0x80) {
            run++;
        }
        if (run) {
            markdown_core_strbuf__grow_by(dest, run);
            if (dest->oom) {
                return;
            }
            unsigned char *out = dest->ptr + dest->size;
            for (bufsize_t i = 0; i < run; i++) {
                uint8_t byte = str[i];
                out[i] = (unsigned char)(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
            }
            dest->size += run;
            dest->ptr[dest->size] = '\0';
            str += run;
            len -= run;
            continue;
        }

        bufsize_t char_len = markdown_core_utf8proc_iterate(str, len, &c);

        if (c >= CF_MAX) {
            markdown_core_strbuf_put(dest, str, char_len);
        } else if (char_len >= 0) {
            uint32_t key = (uint32_t)c;
            uint32_t *entry = bsearch(&key, cf_table, CF_TABLE_SIZE, sizeof(uint32_t), S_case_fold_compare);
            if (entry == NULL) {
                markdown_core_strbuf_put(dest, str, char_len);
            } else {
                markdown_core_strbuf_put(dest, cf_repl + CF_REPL_IDX(*entry), CF_REPL_SIZE(*entry));
            }
        } else {
            encode_unknown(dest);
            char_len = -char_len;
        }

        str += char_len;
        len -= char_len;
    }
}

/* Every category predicate answers ASCII inline from the ctype classes
 * (utf8.h); a scalar above ASCII is looked up here by binary search over the
 * generated Unicode 17 range table of its category. */
MARKDOWN_CORE_DIAGNOSTIC(size_t markdown_core_unicode_range_work;)

static int unicode_range_search(const int32_t (*ranges)[2], size_t count, int32_t uc) {
    size_t low = 0, high = count;
    MARKDOWN_CORE_DIAGNOSTIC(markdown_core_unicode_range_work++;)
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (uc < ranges[mid][0]) {
            high = mid;
        } else if (uc > ranges[mid][1]) {
            low = mid + 1;
        } else {
            return 1;
        }
    }
    return 0;
}

// Zs above ASCII; the ASCII members (TAB, LF, FF, CR, SPACE) answer inline.
int markdown_core_unicode_space(int32_t uc) {
    return uc == 160 || uc == 5760 || (uc >= 8192 && uc <= 8202) || uc == 8239 || uc == 8287 || uc == 12288;
}

#include "unicode_categories.inc"

#include "anchor_scalars.inc"

static int32_t anchor_scalar(int32_t uc) {
    size_t low = 0, high = sizeof(anchor_scalars) / sizeof(*anchor_scalars);
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (uc < anchor_scalars[mid][0]) {
            high = mid;
        } else if (uc > anchor_scalars[mid][1]) {
            low = mid + 1;
        } else {
            return uc + anchor_scalars[mid][2];
        }
    }
    return 0;
}

/* Consume a complete literal here so UTF-8 decoding, scalar projection and
 * encoding share one loop and can be inlined within the Unicode module. */
void markdown_core_utf8proc_anchor(markdown_core_strbuf *dest, const uint8_t *str, bufsize_t len) {
    for (bufsize_t at = 0; at < len;) {
        if (str[at] < 0x80) {
            /* An ASCII run projects byte by byte through the table the
             * generator derived from the same ranges, into room reserved
             * once; a zero removes the byte. */
            bufsize_t end = at + 1;
            while (end < len && str[end] < 0x80) {
                end++;
            }
            markdown_core_strbuf__grow_by(dest, end - at);
            if (dest->oom) {
                return;
            }
            unsigned char *out = dest->ptr + dest->size;
            bufsize_t written = 0;
            for (; at < end; at++) {
                uint8_t byte = anchor_ascii[str[at]];
                if (byte) {
                    out[written++] = byte;
                }
            }
            dest->size += written;
            dest->ptr[dest->size] = '\0';
            continue;
        }
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(str + at, len - at, &scalar);
        assert(width > 0); /* Valid UTF-8 is the parser's input contract. */
        at += width;
        scalar = anchor_scalar(scalar);
        if (scalar) {
            markdown_core_utf8proc_encode_char(scalar, dest);
        }
    }
}
