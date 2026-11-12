#ifndef MARKDOWN_CORE_CLAIM_H
#define MARKDOWN_CORE_CLAIM_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A CLAIM: a pointer slot that readers of a document fill at most once,
 * while other readers of the same document may read or fill it at the same
 * time (markdown_core.h, "A single document"). A reader that finds it empty
 * makes its value and claims the slot with it; the first claim stands, and
 * a reader whose claim did not lets its own value go and reads the one that
 * stands. */

/* The value the slot holds, NULL while it is empty; what the claim that
 * filled it wrote before it is visible after this reads it. */
static MARKDOWN_CORE_INLINE void *markdown_core_claim_read(void *const *slot) {
#if defined(_MSC_VER) && !defined(__clang__)
    return _InterlockedCompareExchangePointer((void *volatile *)slot, NULL, NULL);
#else
    return __atomic_load_n((void **)slot, __ATOMIC_ACQUIRE);
#endif
}

/* Fills the empty slot with `value`, or finds it filled: the value that
 * stands in it, `value` when this claim filled it. */
static MARKDOWN_CORE_INLINE void *markdown_core_claim(void **slot, void *value) {
#if defined(_MSC_VER) && !defined(__clang__)
    void *const found = _InterlockedCompareExchangePointer((void *volatile *)slot, value, NULL);
    return found ? found : value;
#else
    void *expected = NULL;
    return __atomic_compare_exchange_n(slot, &expected, value, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? value
                                                                                                          : expected;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
