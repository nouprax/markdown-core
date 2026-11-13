#ifndef MARKDOWN_CORE_MEASURE_H
#define MARKDOWN_CORE_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* `a` times `b`, both below the prime, modulo it: 2^61 is 1 and 2^64 is 8. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_measure_times(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t low = a0 * b0, middle = a1 * b0 + a0 * b1, high = a1 * b1;
    const uint64_t x = markdown_core_measure_mod(low) + (high << 3) + ((middle & 0x1FFFFFFFu) << 32) + (middle >> 29);
    return markdown_core_measure_mod(x);
}

/* `*measure` with `size` bytes at `bytes` after its own. */
static MARKDOWN_CORE_INLINE void markdown_core_measure_add(markdown_core_measure *measure, const unsigned char *bytes,
                                                           size_t size) {
    for (int lane = 0; lane < MARKDOWN_CORE_MEASURE_LANES; lane++) {
        uint64_t sum = measure->sum[lane], power = measure->power[lane], less = measure->less[lane];
        const uint64_t base = markdown_core_measure_base[lane], inverse = markdown_core_measure_inverse[lane];
        for (size_t i = 0; i < size; i++) {
            sum = markdown_core_measure_mod(sum + markdown_core_measure_times((uint64_t)bytes[i] + 1, power));
            power = markdown_core_measure_times(power, base);
            less = markdown_core_measure_times(less, inverse);
        }
        measure->sum[lane] = sum;
        measure->power[lane] = power;
        measure->less[lane] = less;
    }
    measure->size += (uint32_t)size;
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

#ifdef __cplusplus
}
#endif

#endif
