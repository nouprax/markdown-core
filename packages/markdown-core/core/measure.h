#ifndef MARKDOWN_CORE_MEASURE_H
#define MARKDOWN_CORE_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "buffer.h"
#include "config.h"

#if defined(_MSC_VER) && !defined(__SIZEOF_INT128__)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A MEASURE of bytes: how many there are, `size`, and their `sum`, the
 * bytes read as the digits of one number in base G, the first the least
 * significant, modulo the prime P = 2^61 - 1: byte i weighs G^i. G is a
 * fixed generator of the residues (measure_tables.inc), so no weight comes
 * back short of P - 1 bytes. Two runs of bytes measure as one run of both
 * (markdown_core_measure_join), and a run less its start or its end
 * measures from the measure of the run and the measure of what goes
 * (markdown_core_measure_back, markdown_core_measure_front), each in a
 * product by a power of G and an addition, so a measure of bytes an edit
 * leaves on both sides of what it changes is made of the measures of the
 * two sides and of what it reads (blocks.c, THE SEAMS). The empty run
 * measures NONE.
 *
 * A sum is a hash, not an identity: two runs whose bytes differ can share
 * it. An index of names files them under it (map.h,
 * markdown_core_key_index_hash) and compares the bytes of names whose sums
 * and sizes agree, so a shared sum costs a comparison and nothing else.
 * Changes to bytes that cancel exist (tests/api, heading_names_exact) but
 * take lattice reduction to find; no repeating pattern of them does, as a
 * weight repeats only after P - 1 bytes. */
typedef struct markdown_core_measure {
    uint32_t size;
    uint64_t sum;
} markdown_core_measure;

#define MARKDOWN_CORE_MEASURE_NONE ((markdown_core_measure){0, 0})

#define MARKDOWN_CORE_MEASURE_PRIME ((uint64_t)0x1FFFFFFFFFFFFFFF)

/* The tables measure_tables.inc holds (map.c): b G^k for each byte b at
 * place k of a word, and G and its inverse to each power v 256^j. */
MARKDOWN_CORE_ATTRIBUTE((visibility("hidden"))) extern const uint64_t markdown_core_measure_terms[8][256];
MARKDOWN_CORE_ATTRIBUTE((visibility("hidden"))) extern const uint64_t markdown_core_measure_powers[4][256];
MARKDOWN_CORE_ATTRIBUTE((visibility("hidden"))) extern const uint64_t markdown_core_measure_inverses[4][256];

/* The measure of `size` bytes whose sum is `sum` (no compound literal: the
 * header is read by C++ too). */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_make(uint32_t size, uint64_t sum) {
    markdown_core_measure measure;
    measure.size = size;
    measure.sum = sum;
    return measure;
}

/* `x`, below 2^64, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_mod(uint64_t x) {
    x = (x & MARKDOWN_CORE_MEASURE_PRIME) + (x >> 61);
    return x >= MARKDOWN_CORE_MEASURE_PRIME ? x - MARKDOWN_CORE_MEASURE_PRIME : x;
}

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 markdown_core_measure_wide;
#endif

/* `a`, below 2^63, times `b`, below the prime, folded below 2^61 + 4 but
 * not reduced, as a run's sum is kept while it is measured: the product's
 * bits past 61 weigh 2^61, which is 1. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_fold(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    const markdown_core_measure_wide product = (markdown_core_measure_wide)a * b;
    const uint64_t low = (uint64_t)product, high = (uint64_t)(product >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    uint64_t high;
    const uint64_t low = _umul128(a, b, &high);
#elif defined(_MSC_VER) && defined(_M_ARM64)
    const uint64_t low = a * b, high = __umulh(a, b);
#else
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0;
    const uint64_t middle = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    const uint64_t low = middle << 32 | (uint32_t)p00, high = a1 * b1 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
#endif
    const uint64_t x = (low & MARKDOWN_CORE_MEASURE_PRIME) + (high << 3 | low >> 61);
    return (x & MARKDOWN_CORE_MEASURE_PRIME) + (x >> 61);
}

/* `a` times `b`, both below the prime, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_times(uint64_t a, uint64_t b) {
    return markdown_core_measure_mod(markdown_core_measure_fold(a, b));
}

/* `x` times `table`'s power for `size`, one byte of `size` at a time. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_power(uint64_t x, uint64_t size,
                                                                 const uint64_t (*table)[256]) {
    for (unsigned j = 0; size; j++, size >>= 8) {
        if (size & 0xFF) {
            x = markdown_core_measure_times(x, table[j][size & 0xFF]);
        }
    }
    return x;
}

/* `x`, below the prime, times the weight of the first byte past `size`
 * bytes, G^size, modulo the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_turn(uint64_t x, uint64_t size) {
    return markdown_core_measure_power(x, size, markdown_core_measure_powers);
}

/* `x`, below the prime, divided by that weight. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_unturn(uint64_t x, uint64_t size) {
    return markdown_core_measure_power(x, size, markdown_core_measure_inverses);
}

/* The sum of the `count` bytes at `bytes`, below eight, as the first
 * `count` places of a word weigh them: below seven times the prime. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_few(const unsigned char *bytes, size_t count) {
    uint64_t sum = 0;
    for (size_t k = 0; k < count; k++) {
        sum += markdown_core_measure_terms[k][bytes[k]];
    }
    return sum;
}

/* The eight bytes at `bytes` as a word weighs them, folded below 2^61 + 8:
 * each place's weight is below the prime, so their sum is below 2^64. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_word(const unsigned char *bytes) {
    const uint64_t (*w)[256] = markdown_core_measure_terms;
    const uint64_t sum = w[0][bytes[0]] + w[1][bytes[1]] + w[2][bytes[2]] + w[3][bytes[3]] + w[4][bytes[4]] +
                         w[5][bytes[5]] + w[6][bytes[6]] + w[7][bytes[7]];
    return (sum & MARKDOWN_CORE_MEASURE_PRIME) + (sum >> 61);
}

/* The measure of the `size` bytes at `bytes`, eight at a time from the
 * last: the run so far moves a word up (G^8) as the word before it comes
 * in. The sum stays below 2^62 until the end, each product folding its
 * high bits once. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_of(const unsigned char *bytes, size_t size) {
    const size_t steps = size / 8, rest = size - steps * 8;
    const uint64_t up = markdown_core_measure_powers[0][8];
    uint64_t sum = markdown_core_measure_few(bytes + steps * 8, rest);
    sum = (sum & MARKDOWN_CORE_MEASURE_PRIME) + (sum >> 61);
    for (size_t k = steps; k-- > 0;) {
        sum = markdown_core_measure_fold(sum, up) + markdown_core_measure_word(bytes + 8 * k);
    }
    return markdown_core_measure_make((uint32_t)size, markdown_core_measure_mod(sum));
}

/* The bytes `a` measures, then those `b` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_join(markdown_core_measure a,
                                                                             markdown_core_measure b) {
    return markdown_core_measure_make(a.size + b.size,
                                      markdown_core_measure_mod(a.sum + markdown_core_measure_turn(b.sum, a.size)));
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
    return markdown_core_measure_make(whole.size - front.size, markdown_core_measure_unturn(rest, front.size));
}

/* The bytes `whole` measures before those its end `back` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_front(markdown_core_measure back,
                                                                              markdown_core_measure whole) {
    const uint32_t size = whole.size - back.size;
    const uint64_t lead = markdown_core_measure_turn(back.sum, size);
    return markdown_core_measure_make(size, markdown_core_measure_mod(whole.sum + MARKDOWN_CORE_MEASURE_PRIME - lead));
}

#ifdef __cplusplus
}
#endif

#endif
