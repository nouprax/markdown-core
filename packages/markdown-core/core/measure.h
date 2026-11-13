#ifndef MARKDOWN_CORE_MEASURE_H
#define MARKDOWN_CORE_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A MEASURE of bytes: how many there are, `size`, and in each of two lanes
 * their sum, the polynomial hash sum of b[i] + 1 times the lane's base P to
 * the power i, modulo the prime 2^61 - 1, with P to the power `size` and
 * its inverse, `power` and `less`. Two runs of bytes measure as one run of
 * both (markdown_core_measure_join), and a run less its start or its end
 * measures from the measure of the run and the measure of what goes
 * (markdown_core_measure_back, markdown_core_measure_front), each in a few
 * products, so a measure of bytes an edit leaves on both sides of what it
 * changes is made of the measures of the two sides and of what it reads
 * (blocks.c, THE SEAMS). The empty run measures NONE.
 *
 * Two lanes of two bases make two different runs of n bytes measure the
 * same by chance about (n / 2^61)^2 of the time; the bases are fixed, so
 * a document could be made to hold two such runs on purpose (heading.c, A
 * LONG NAME, says what it costs). */
enum { MARKDOWN_CORE_MEASURE_LANES = 2 };

typedef struct markdown_core_measure {
    uint32_t size;
    uint64_t sum[MARKDOWN_CORE_MEASURE_LANES], power[MARKDOWN_CORE_MEASURE_LANES], less[MARKDOWN_CORE_MEASURE_LANES];
} markdown_core_measure;

#define MARKDOWN_CORE_MEASURE_NONE ((markdown_core_measure){0, {0, 0}, {1, 1}, {1, 1}})

#define MARKDOWN_CORE_MEASURE_PRIME ((uint64_t)0x1FFFFFFFFFFFFFFF)
/* Each lane's base P, and the P' for which P times P' is 1 modulo the
 * prime. */
static const uint64_t markdown_core_measure_base[MARKDOWN_CORE_MEASURE_LANES] = {0x0545F4914F6CDD1Eu,
                                                                                 0x07B584C010672A4Du};
static const uint64_t markdown_core_measure_inverse[MARKDOWN_CORE_MEASURE_LANES] = {0x16783E98E07BC190u,
                                                                                    0x018D5A541CBF2989u};

/* `x`, below 2^64, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_mod(uint64_t x) {
    x = (x & MARKDOWN_CORE_MEASURE_PRIME) + (x >> 61);
    return x >= MARKDOWN_CORE_MEASURE_PRIME ? x - MARKDOWN_CORE_MEASURE_PRIME : x;
}

/* `a` times `b`, both below the prime, modulo it: 2^61 is 1, and 2^64 is 8
 * where there is no product of 128 bits. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_times(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 markdown_core_measure_wide;
    const markdown_core_measure_wide x = (markdown_core_measure_wide)a * b;
    return markdown_core_measure_mod(((uint64_t)x & MARKDOWN_CORE_MEASURE_PRIME) + (uint64_t)(x >> 61));
#else
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t low = a0 * b0, middle = a1 * b0 + a0 * b1, high = a1 * b1;
    const uint64_t x = markdown_core_measure_mod(low) + (high << 3) + ((middle & 0x1FFFFFFFu) << 32) + (middle >> 29);
    return markdown_core_measure_mod(x);
#endif
}

/* `base`, below the prime, to the power `exponent`, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_raise(uint64_t base, uint64_t exponent) {
    uint64_t result = 1;
    for (; exponent; exponent >>= 1) {
        if (exponent & 1) {
            result = markdown_core_measure_times(result, base);
        }
        base = markdown_core_measure_times(base, base);
    }
    return result;
}

/* `*measure` with `size` bytes at `bytes` after its own: their sum by
 * Horner's rule from the last, one product a byte in each lane, and the
 * powers they add by squaring. */
static MARKDOWN_CORE_INLINE void markdown_core_measure_add(markdown_core_measure *measure, const unsigned char *bytes,
                                                           size_t size) {
    if (!size) {
        return;
    }
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        const uint64_t base = markdown_core_measure_base[lane];
        uint64_t sum = 0;
        for (size_t i = size; i-- > 0;) {
            sum = markdown_core_measure_mod(markdown_core_measure_times(sum, base) + (uint64_t)bytes[i] + 1);
        }
        measure->sum[lane] =
            markdown_core_measure_mod(measure->sum[lane] + markdown_core_measure_times(sum, measure->power[lane]));
        measure->power[lane] =
            markdown_core_measure_times(measure->power[lane], markdown_core_measure_raise(base, (uint64_t)size));
        measure->less[lane] = markdown_core_measure_times(
            measure->less[lane], markdown_core_measure_raise(markdown_core_measure_inverse[lane], (uint64_t)size));
    }
    measure->size += (uint32_t)size;
}

/* The sums alone of `size` bytes, in `*measure`, which measures none:
 * Horner's rule in both lanes at once, each step's sum not brought below the
 * prime until the last. As each base is below 2^59, a sum below 2^63 times
 * it folds back to below 2^62 plus a byte. */
static MARKDOWN_CORE_INLINE void markdown_core_measure_sums(markdown_core_measure *measure, const unsigned char *bytes,
                                                            size_t size) {
    uint64_t first = 0, second = 0;
    const uint64_t one = markdown_core_measure_base[0], two = markdown_core_measure_base[1];
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 markdown_core_measure_wide;
#define MARKDOWN_CORE_MEASURE_STEP(at)                                                                                 \
    do {                                                                                                               \
        const markdown_core_measure_wide x = (markdown_core_measure_wide)first * one,                                  \
                                         y = (markdown_core_measure_wide)second * two;                                 \
        const uint64_t c = (uint64_t)bytes[at] + 1;                                                                    \
        first = ((uint64_t)x & MARKDOWN_CORE_MEASURE_PRIME) + (uint64_t)(x >> 61) + c;                                 \
        second = ((uint64_t)y & MARKDOWN_CORE_MEASURE_PRIME) + (uint64_t)(y >> 61) + c;                                \
    } while (0)
#else
#define MARKDOWN_CORE_MEASURE_STEP(at)                                                                                 \
    do {                                                                                                               \
        first = markdown_core_measure_mod(markdown_core_measure_times(first, one) + bytes[at] + 1);                    \
        second = markdown_core_measure_mod(markdown_core_measure_times(second, two) + bytes[at] + 1);                  \
    } while (0)
#endif
    /* Four bytes a turn, the last first. */
    size_t i = size;
    for (; i >= 4; i -= 4) {
        MARKDOWN_CORE_MEASURE_STEP(i - 1);
        MARKDOWN_CORE_MEASURE_STEP(i - 2);
        MARKDOWN_CORE_MEASURE_STEP(i - 3);
        MARKDOWN_CORE_MEASURE_STEP(i - 4);
    }
    while (i-- > 0) {
        MARKDOWN_CORE_MEASURE_STEP(i);
    }
#undef MARKDOWN_CORE_MEASURE_STEP
    measure->sum[0] = markdown_core_measure_mod(first);
    measure->sum[1] = markdown_core_measure_mod(second);
    measure->size = (uint32_t)size;
}

/* The bytes `a` measures, then those `b` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_join(markdown_core_measure a,
                                                                             markdown_core_measure b) {
    markdown_core_measure measure;
    measure.size = a.size + b.size;
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        measure.sum[lane] =
            markdown_core_measure_mod(a.sum[lane] + markdown_core_measure_times(b.sum[lane], a.power[lane]));
        measure.power[lane] = markdown_core_measure_times(a.power[lane], b.power[lane]);
        measure.less[lane] = markdown_core_measure_times(a.less[lane], b.less[lane]);
    }
    return measure;
}

/* The bytes `whole` measures past those its start `front` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_back(markdown_core_measure front,
                                                                             markdown_core_measure whole) {
    markdown_core_measure measure;
    measure.size = whole.size - front.size;
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        const uint64_t rest =
            markdown_core_measure_mod(whole.sum[lane] + MARKDOWN_CORE_MEASURE_PRIME - front.sum[lane]);
        measure.sum[lane] = markdown_core_measure_times(rest, front.less[lane]);
        measure.power[lane] = markdown_core_measure_times(whole.power[lane], front.less[lane]);
        measure.less[lane] = markdown_core_measure_times(whole.less[lane], front.power[lane]);
    }
    return measure;
}

/* The bytes `whole` measures before those its end `back` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_front(markdown_core_measure back,
                                                                              markdown_core_measure whole) {
    markdown_core_measure measure;
    measure.size = whole.size - back.size;
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        const uint64_t power = markdown_core_measure_times(whole.power[lane], back.less[lane]);
        const uint64_t lead = markdown_core_measure_times(back.sum[lane], power);
        measure.sum[lane] = markdown_core_measure_mod(whole.sum[lane] + MARKDOWN_CORE_MEASURE_PRIME - lead);
        measure.power[lane] = power;
        measure.less[lane] = markdown_core_measure_times(whole.less[lane], back.power[lane]);
    }
    return measure;
}

/* A LONG NAME'S KEY. A name -- an anchor a heading computes or one a node
 * authors -- whose bytes before its suffixes (the `-k` groups a heading's
 * name may end in, heading.c, stem_parent) are more than
 * MARKDOWN_CORE_NAME_WHOLE is known by its key: MARKDOWN_CORE_NAME_MARK, the
 * two sums of the measure of those bytes and its size in hex, then the
 * suffixes; every index of anchors holds the key (heading.c, A LONG NAME).
 * A name of no more bytes is read whole at little cost, and no measure is
 * made of it: an anchor or a heading of common length costs nothing more.
 * Two different stems share a key only when both lanes of their measures
 * agree: by chance about (n / 2^61)^2 of the time for n bytes, or when a
 * document is made to, as the bases are fixed. No computed name holds the
 * mark, which a heading's projection drops as it drops every control; an
 * authored one that starts with it is known by its key whatever its length
 * (markdown_core_name_keyed), so that no name's bytes are another's key. */
enum {
    MARKDOWN_CORE_NAME_WHOLE = 1024,
    MARKDOWN_CORE_NAME_SUMS = 1 + 16 * MARKDOWN_CORE_MEASURE_LANES,
    MARKDOWN_CORE_NAME_KEY = MARKDOWN_CORE_NAME_SUMS + 8
};
#define MARKDOWN_CORE_NAME_MARK '\x01'

/* The bytes of the base that stand before its suffixes: of `size` bytes at
 * `bytes`, which stand `offset` bytes into the base. SIZE_MAX when the
 * bytes before them may hold more of the suffixes. */
static MARKDOWN_CORE_INLINE size_t markdown_core_name_stem(const unsigned char *bytes, size_t size, size_t offset) {
    size_t stem = size;
    for (;;) {
        if (!stem) {
            return offset ? SIZE_MAX : 0;
        }
        size_t at = stem;
        while (at > 0 && bytes[at - 1] >= '0' && bytes[at - 1] <= '9') {
            at--;
        }
        if (!at && offset) {
            return SIZE_MAX;
        }
        if (at == stem || bytes[at] == '0' || offset + at < 2 || bytes[at - 1] != '-') {
            return stem;
        }
        stem = at - 1;
    }
}

/* The key of the base whose stem measures `measure`, its suffixes the
 * `size` bytes of `name` from `from` on, which `name` holds then. */
static MARKDOWN_CORE_INLINE void markdown_core_name_mark(markdown_core_strbuf *name, markdown_core_measure measure,
                                                         bufsize_t from, bufsize_t size) {
    static const char hex[] = "0123456789abcdef";
    unsigned char key[MARKDOWN_CORE_NAME_KEY];
    key[0] = (unsigned char)MARKDOWN_CORE_NAME_MARK;
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        for (int i = 0; i < 16; i++) {
            key[1 + 16 * lane + i] = (unsigned char)hex[(measure.sum[lane] >> (60 - 4 * i)) & 15];
        }
    }
    for (int i = 0; i < 8; i++) {
        key[MARKDOWN_CORE_NAME_SUMS + i] = (unsigned char)hex[(measure.size >> (28 - 4 * i)) & 15];
    }
    markdown_core_strbuf_grow(name, MARKDOWN_CORE_NAME_KEY + size);
    if (name->oom) {
        return;
    }
    memmove(name->ptr + MARKDOWN_CORE_NAME_KEY, name->ptr + from, (size_t)size);
    memcpy(name->ptr, key, MARKDOWN_CORE_NAME_KEY);
    name->size = MARKDOWN_CORE_NAME_KEY + size;
    name->ptr[name->size] = '\0';
}

/* Whether the name of `size` bytes at `bytes` whose stem is `stem` bytes is
 * known by its key: a long one, and one that starts with the mark. */
static MARKDOWN_CORE_INLINE bool markdown_core_name_keyed(const unsigned char *bytes, size_t size, size_t stem) {
    return stem > MARKDOWN_CORE_NAME_WHOLE || (size && bytes[0] == (unsigned char)MARKDOWN_CORE_NAME_MARK);
}

/* The base `name` holds, its key: a long one by its measure, and one that
 * starts with the mark. */
static MARKDOWN_CORE_INLINE void markdown_core_name_key(markdown_core_strbuf *name) {
    const size_t stem = markdown_core_name_stem(name->ptr, (size_t)name->size, 0);
    if (!markdown_core_name_keyed(name->ptr, (size_t)name->size, stem)) {
        return;
    }
    markdown_core_measure measure = {0, {0, 0}, {1, 1}, {1, 1}};
    markdown_core_measure_sums(&measure, name->ptr, stem);
    markdown_core_name_mark(name, measure, (bufsize_t)stem, name->size - (bufsize_t)stem);
}

#ifdef __cplusplus
}
#endif

#endif
