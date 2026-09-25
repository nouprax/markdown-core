#ifndef MARKDOWN_CORE_SLAB_H
#define MARKDOWN_CORE_SLAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FIXED-SIZE SLOTS, TAKEN FROM SLABS, for the objects a parse makes many of
 * and its tree keeps after it: nodes, and the resources links read through
 * (node.h).
 *
 * A slot is a header naming the slab it came from, then storage for one
 * object. A parse takes slots from SLABS -- one allocation of many slots --
 * through a pool it owns, and a caller with no parse takes one slot from the
 * allocator; the header says which, so an object is released the same way
 * whichever storage it came from, and nothing about the storage is visible
 * through the object.
 *
 * A slab lives while anything holds it: every slot taken from it, and the
 * pool while that slab is the one it takes slots from. A slot released into a
 * pool goes back to it and is handed out again before another slot is taken
 * from a slab, keeping its hold until the pool is disposed; a slot released
 * with no pool drops its hold, and the slab is freed with its last one -- by
 * whichever release that turns out to be. Disposing the pool drops only the
 * holds the pool itself has, so what the parse built outlives the parse and
 * keeps its slabs. Slots of one slab are released from one thread at a time;
 * two parses never share a slab.
 *
 * A pool holds one kind of object. Its slot size and slab size are constants
 * that kind's code passes to every call, so the arithmetic folds at each
 * call and the pool itself is only its state, which starts zeroed. */

typedef struct markdown_core_slab {
    union {
        size_t holds;
        long double alignment;
    } head;
} markdown_core_slab;

/* What each slot begins with: the slab it came from, or NULL for a slot of
 * its own from the allocator. It is the slot's, not the slab's -- the slab
 * begins with its hold count above. Padded to scalar alignment, so the
 * storage after it is aligned for any ordinary field. */
typedef union {
    markdown_core_slab *slab;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_slot_header;

typedef struct markdown_core_slab_pool {
    /* The slab slots are being taken from, held by the pool. */
    markdown_core_slab *current;
    /* Slots of `current` already taken, from its start. */
    size_t taken;
    /* Storage of slots released into the pool, linked through its first
     * bytes, reused before another slot is taken from a slab. */
    void *released;
} markdown_core_slab_pool;

/* How far apart a slab's slots of `bytes` of storage are. */
#define MARKDOWN_CORE_SLOT_STRIDE(bytes)                                                                               \
    (sizeof(markdown_core_slot_header) + ((bytes) + sizeof(markdown_core_slot_header) - 1) /                           \
                                             sizeof(markdown_core_slot_header) * sizeof(markdown_core_slot_header))

/* Starts a slab of `slab_bytes` for the pool to take from, dropping the
 * pool's hold on the one it replaces. False, leaving the pool as it was, when
 * the slab cannot be allocated. */
bool markdown_core_slab_pool_grow(markdown_core_slab_pool *pool, size_t slab_bytes);

/* Drops what the pool holds: its released slots and its current slab. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_slab_pool_dispose(markdown_core_slab_pool *pool);

static inline void markdown_core_slab_drop(markdown_core_slab *slab) {
    if (slab && --slab->head.holds == 0) {
        markdown_core_free(slab);
    }
}

/* Uninitialized storage for one object of `bytes`, from the pool's released
 * slots, then its current slab, then a new slab of `slab_bytes`; or one slot
 * from the allocator when there is no pool. NULL when none can be had. */
static inline void *markdown_core_slab_take(markdown_core_slab_pool *pool, size_t bytes, size_t slab_bytes) {
    markdown_core_slot_header *slot;
    if (!pool) {
        slot = (markdown_core_slot_header *)markdown_core_realloc(NULL, sizeof(*slot) + bytes);
        if (!slot) {
            return NULL;
        }
        slot->slab = NULL;
        return slot + 1;
    }
    if (pool->released) {
        void *storage = pool->released;
        memcpy(&pool->released, storage, sizeof(pool->released));
        return storage;
    }
    if ((!pool->current ||
         pool->taken == (slab_bytes - sizeof(markdown_core_slab)) / MARKDOWN_CORE_SLOT_STRIDE(bytes)) &&
        !markdown_core_slab_pool_grow(pool, slab_bytes)) {
        return NULL;
    }
    slot = (markdown_core_slot_header *)((unsigned char *)(pool->current + 1) +
                                         pool->taken++ * MARKDOWN_CORE_SLOT_STRIDE(bytes));
    slot->slab = pool->current;
    pool->current->head.holds++;
    return slot + 1;
}

/* Gives back the slot whose storage is `storage`, whatever is in it: into the
 * pool for reuse, or, with no pool, dropping its slab hold. A slot from the
 * allocator is freed. */
static inline void markdown_core_slab_release(markdown_core_slab_pool *pool, void *storage) {
    markdown_core_slot_header *slot = (markdown_core_slot_header *)storage - 1;
    if (!slot->slab) {
        markdown_core_free(slot);
    } else if (pool) {
        memcpy(storage, &pool->released, sizeof(pool->released));
        pool->released = storage;
    } else {
        markdown_core_slab_drop(slot->slab);
    }
}

#ifdef __cplusplus
}
#endif

#endif
