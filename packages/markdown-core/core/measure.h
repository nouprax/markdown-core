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
 * it, by chance or by design, as two bytes 61 apart weigh the same. An
 * index of names files them under it (map.h, markdown_core_key_index_hash)
 * and compares the bytes of names whose sums and sizes agree, so a shared
 * sum costs a comparison and nothing else. */
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
 * least significant: four to seven as two overlapping halves, fewer as
 * their first, middle and last, each where it stands. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_half(const unsigned char *bytes) {
    uint32_t half;
    memcpy(&half, bytes, sizeof(half));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    half = __builtin_bswap32(half);
#endif
    return half;
}
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_few(const unsigned char *bytes, size_t count) {
    if (count >= 4) {
        return markdown_core_measure_half(bytes) | (markdown_core_measure_half(bytes + count - 4) >> (8 * (8 - count)))
                                                       << 32;
    }
    if (!count) {
        return 0;
    }
    return (uint64_t)bytes[0] | (uint64_t)bytes[count / 2] << (8 * (count / 2)) |
           (uint64_t)bytes[count - 1] << (8 * (count - 1));
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

#ifdef __cplusplus
}
#endif

#endif
