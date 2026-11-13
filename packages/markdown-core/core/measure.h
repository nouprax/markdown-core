#ifndef MARKDOWN_CORE_MEASURE_H
#define MARKDOWN_CORE_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A MEASURE of bytes: how many there are, `size`, and their sum, `sum`, the
 * polynomial hash sum of b[i] + 1 times P to the power i, modulo the prime
 * 2^61 - 1, with P to the power `size` and its inverse, `power` and `less`.
 * Two runs of bytes measure as one run of both (markdown_core_measure_join),
 * and a run less its start or its end measures from the measure of the run
 * and the measure of what goes (markdown_core_measure_back,
 * markdown_core_measure_front), each in a few products, so a measure of
 * bytes an edit leaves on both sides of what it changes is made of the
 * measures of the two sides and of what it reads (blocks.c, THE SEAMS). The
 * empty run measures {0, 0, 1, 1}. */
typedef struct markdown_core_measure {
    uint32_t size;
    uint64_t sum, power, less;
} markdown_core_measure;

#define MARKDOWN_CORE_MEASURE_NONE ((markdown_core_measure){0, 0, 1, 1})

#define MARKDOWN_CORE_MEASURE_PRIME ((uint64_t)0x1FFFFFFFFFFFFFFF)
/* P, and the P' for which P times P' is 1 modulo the prime. */
#define MARKDOWN_CORE_MEASURE_BASE ((uint64_t)0x0545F4914F6CDD1E)
#define MARKDOWN_CORE_MEASURE_INVERSE ((uint64_t)0x16783E98E07BC190)

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
    uint64_t sum = measure->sum, power = measure->power, less = measure->less;
    for (size_t i = 0; i < size; i++) {
        sum = markdown_core_measure_mod(sum + markdown_core_measure_times((uint64_t)bytes[i] + 1, power));
        power = markdown_core_measure_times(power, MARKDOWN_CORE_MEASURE_BASE);
        less = markdown_core_measure_times(less, MARKDOWN_CORE_MEASURE_INVERSE);
    }
    *measure = (markdown_core_measure){measure->size + (uint32_t)size, sum, power, less};
}

/* The bytes `a` measures, then those `b` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_join(markdown_core_measure a,
                                                                             markdown_core_measure b) {
    return (markdown_core_measure){
        a.size + b.size, markdown_core_measure_mod(a.sum + markdown_core_measure_times(b.sum, a.power)),
        markdown_core_measure_times(a.power, b.power), markdown_core_measure_times(a.less, b.less)};
}

/* The bytes `whole` measures past those its start `front` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_back(markdown_core_measure front,
                                                                             markdown_core_measure whole) {
    const uint64_t rest = markdown_core_measure_mod(whole.sum + MARKDOWN_CORE_MEASURE_PRIME - front.sum);
    return (markdown_core_measure){whole.size - front.size, markdown_core_measure_times(rest, front.less),
                                   markdown_core_measure_times(whole.power, front.less),
                                   markdown_core_measure_times(whole.less, front.power)};
}

/* The bytes `whole` measures before those its end `back` measures. */
static MARKDOWN_CORE_INLINE markdown_core_measure markdown_core_measure_front(markdown_core_measure back,
                                                                              markdown_core_measure whole) {
    const uint64_t power = markdown_core_measure_times(whole.power, back.less);
    const uint64_t lead = markdown_core_measure_times(back.sum, power);
    return (markdown_core_measure){whole.size - back.size,
                                   markdown_core_measure_mod(whole.sum + MARKDOWN_CORE_MEASURE_PRIME - lead), power,
                                   markdown_core_measure_times(whole.less, back.power)};
}

#ifdef __cplusplus
}
#endif

#endif
