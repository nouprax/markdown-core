#ifndef MARKDOWN_CORE_ATOMIC_H
#define MARKDOWN_CORE_ATOMIC_H

#include <stdint.h>
#include "markdown-core.h"

/* The one atomic the engine needs: a 32-bit reference count (#153, owner
 * ruling: C11 atomics). Increments are relaxed -- publication of the counted
 * object to another thread is the consumer's synchronization point, not the
 * count's -- and decrements are acquire-release so the final decrement
 * observes every write made under the references it ends, which is what
 * makes the free after it sound. MSVC's C mode predates usable stdatomic,
 * so it gets the Interlocked intrinsics (sequentially consistent, which is
 * strictly stronger) behind the same three operations. */
#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

typedef long volatile markdown_core_atomic_u32;

static __inline void markdown_core_atomic_init(markdown_core_atomic_u32 *count, uint32_t value) {
    *count = (long)value;
}

/* Both return the count AFTER the operation, so `decrement == 0` names the
 * caller that frees. */
static __inline uint32_t markdown_core_atomic_increment(markdown_core_atomic_u32 *count) {
    return (uint32_t)_InterlockedIncrement(count);
}

static __inline uint32_t markdown_core_atomic_decrement(markdown_core_atomic_u32 *count) {
    return (uint32_t)_InterlockedDecrement(count);
}

#else

#include <stdatomic.h>

typedef _Atomic uint32_t markdown_core_atomic_u32;

static MARKDOWN_CORE_INLINE void markdown_core_atomic_init(markdown_core_atomic_u32 *count, uint32_t value) {
    atomic_init(count, value);
}

/* Both return the count AFTER the operation, so `decrement == 0` names the
 * caller that frees. */
static MARKDOWN_CORE_INLINE uint32_t markdown_core_atomic_increment(markdown_core_atomic_u32 *count) {
    return (uint32_t)atomic_fetch_add_explicit(count, 1u, memory_order_relaxed) + 1u;
}

static MARKDOWN_CORE_INLINE uint32_t markdown_core_atomic_decrement(markdown_core_atomic_u32 *count) {
    return (uint32_t)atomic_fetch_sub_explicit(count, 1u, memory_order_acq_rel) - 1u;
}

#endif

#endif
