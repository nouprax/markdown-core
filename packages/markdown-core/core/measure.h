#ifndef MARKDOWN_CORE_MEASURE_H
#define MARKDOWN_CORE_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "buffer.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A MEASURE of bytes: how many there are, `size`, and their `sum`, the
 * bytes read as one number, the first the least significant, modulo the
 * prime 2^61 - 1: byte i weighs 2^(8 i), and as 2^61 is 1 modulo the
 * prime, a weight is a turn of 61 bits. Two runs of bytes measure as one
 * run of both (markdown_core_measure_join), and a run less its start or its
 * end measures from the measure of the run and the measure of what goes
 * (markdown_core_measure_back, markdown_core_measure_front), each in a
 * turn and an addition, so a measure of bytes an edit leaves on both sides
 * of what it changes is made of the measures of the two sides and of what
 * it reads (blocks.c, THE SEAMS). The empty run measures NONE.
 *
 * A sum is a hash, not an identity: two runs whose bytes differ can share
 * it, by chance or by design, as two bytes 61 apart weigh the same. Every
 * index of keys reads it (map.c) and compares the bytes of keys whose sums
 * and sizes agree, so a shared sum costs a comparison and nothing else. */
typedef struct markdown_core_measure {
    uint32_t size;
    uint64_t sum;
} markdown_core_measure;

#define MARKDOWN_CORE_MEASURE_NONE ((markdown_core_measure){0, 0})

#define MARKDOWN_CORE_MEASURE_PRIME ((uint64_t)0x1FFFFFFFFFFFFFFF)

/* `x`, below 2^64, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_mod(uint64_t x) {
    x = (x & MARKDOWN_CORE_MEASURE_PRIME) + (x >> 61);
    return x >= MARKDOWN_CORE_MEASURE_PRIME ? x - MARKDOWN_CORE_MEASURE_PRIME : x;
}

/* `x`, below the prime, times the weight of the first byte past `size`
 * bytes, modulo the prime: `x` turned by 8 `size` bits of 61. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_turn(uint64_t x, uint64_t size) {
    const unsigned turn = (unsigned)((size % 61) * 8 % 61);
    return turn ? ((x << turn) & MARKDOWN_CORE_MEASURE_PRIME) | (x >> (61 - turn)) : x;
}

/* `x`, below the prime, divided by that weight: turned back. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_unturn(uint64_t x, uint64_t size) {
    const unsigned turn = (unsigned)((size % 61) * 8 % 61);
    return turn ? (x >> turn) | ((x << (61 - turn)) & MARKDOWN_CORE_MEASURE_PRIME) : x;
}

/* The `count` bytes at `bytes`, below eight, as a number, the first the
 * least significant. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_few(const unsigned char *bytes, size_t count) {
    uint64_t word = 0;
    for (size_t i = count; i-- > 0;) {
        word = word << 8 | bytes[i];
    }
    return word;
}

/* The eight bytes at `bytes` as a number, the first the least
 * significant. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_word(const unsigned char *bytes) {
    uint64_t word;
    memcpy(&word, bytes, sizeof(word));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    word = __builtin_bswap64(word);
#endif
    return word;
}

/* The measure of `size` bytes at `bytes`: eight bytes a step from the
 * last, after the few past the last whole eight. A step times the sum by
 * 2^64, which is 8 modulo the prime, and adds a word: the bits of each past
 * the 61st count one apiece, so the sum stays below 2^62 + 72 for any sum
 * before, and one fold at the end brings it below the prime. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_of(const unsigned char *bytes, size_t size) {
    const size_t steps = size / 8, rest = size - steps * 8;
    uint64_t sum = !rest       ? 0
                   : size >= 8 ? markdown_core_measure_word(bytes + size - 8) >> (8 * (8 - rest))
                               : markdown_core_measure_few(bytes, rest);
    for (size_t k = steps; k-- > 0;) {
        const uint64_t word = markdown_core_measure_word(bytes + 8 * k);
        sum = ((sum << 3) & MARKDOWN_CORE_MEASURE_PRIME) + (sum >> 58) + (word & MARKDOWN_CORE_MEASURE_PRIME) +
              (word >> 61);
    }
    return (markdown_core_measure){(uint32_t)size, markdown_core_measure_mod(sum)};
}

/* The bytes `a` measures, then those `b` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_join(markdown_core_measure a,
                                                                             markdown_core_measure b) {
    return (markdown_core_measure){a.size + b.size,
                                   markdown_core_measure_mod(a.sum + markdown_core_measure_turn(b.sum, a.size))};
}

/* `*measure` with `size` bytes at `bytes` after its own. */
static MARKDOWN_CORE_INLINE void markdown_core_measure_add(markdown_core_measure *measure, const unsigned char *bytes,
                                                           size_t size) {
    *measure = markdown_core_measure_join(*measure, markdown_core_measure_of(bytes, size));
}

/* The bytes `whole` measures past those its start `front` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_back(markdown_core_measure front,
                                                                             markdown_core_measure whole) {
    const uint64_t rest = markdown_core_measure_mod(whole.sum + MARKDOWN_CORE_MEASURE_PRIME - front.sum);
    return (markdown_core_measure){whole.size - front.size, markdown_core_measure_unturn(rest, front.size)};
}

/* The bytes `whole` measures before those its end `back` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_front(markdown_core_measure back,
                                                                              markdown_core_measure whole) {
    const uint32_t size = whole.size - back.size;
    const uint64_t lead = markdown_core_measure_turn(back.sum, size);
    return (markdown_core_measure){size, markdown_core_measure_mod(whole.sum + MARKDOWN_CORE_MEASURE_PRIME - lead)};
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
    MARKDOWN_CORE_NAME_SUMS = 1 + 16,
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
    for (int i = 0; i < 16; i++) {
        key[1 + i] = (unsigned char)hex[(measure.sum >> (60 - 4 * i)) & 15];
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
    markdown_core_name_mark(name, markdown_core_measure_of(name->ptr, stem), (bufsize_t)stem,
                            name->size - (bufsize_t)stem);
}

#ifdef __cplusplus
}
#endif

#endif
