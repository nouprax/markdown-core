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
    /* Slots of `current` already taken, from its start; bytes, for the slabs
     * of a pool of varied sizes. */
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

/* STORAGE OF VARIED SIZES, for the objects a parse makes many of whose
 * sizes differ. A size is rounded up to a CLASS, a multiple of
 * MARKDOWN_CORE_BYTES_CLASS bytes, and storage of each class is a slot like
 * any other: taken from a slab, and released into the pool to be handed out
 * again for that class before a slab is cut further. Storage larger than the
 * largest class is a slot of its own from the allocator, as it is with no
 * pool. A slot's header names its slab and its class. */
#define MARKDOWN_CORE_BYTES_CLASS ((size_t)16)
#define MARKDOWN_CORE_BYTES_CLASSES 32

typedef union {
    struct {
        markdown_core_slab *slab;
        size_t size_class;
    } owner;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_bytes_header;

typedef struct markdown_core_bytes_pool {
    /* The slab storage is cut from, and how many bytes of it are cut. */
    markdown_core_slab_pool slabs;
    /* Released storage of each class, linked through its first bytes. */
    void *released[MARKDOWN_CORE_BYTES_CLASSES];
} markdown_core_bytes_pool;

/* Uninitialized storage of `bytes`, from the pool's released storage of its
 * class, then its current slab, then a new slab of `slab_bytes`; or a slot
 * of its own from the allocator. NULL when none can be had. */
static inline void *markdown_core_bytes_take(markdown_core_bytes_pool *pool, size_t bytes, size_t slab_bytes) {
    const size_t size_class = (bytes + MARKDOWN_CORE_BYTES_CLASS - 1) / MARKDOWN_CORE_BYTES_CLASS;
    markdown_core_bytes_header *header;
    if (!pool || !size_class || size_class > MARKDOWN_CORE_BYTES_CLASSES) {
        header = (markdown_core_bytes_header *)markdown_core_realloc(NULL, sizeof(*header) + bytes);
        if (!header) {
            return NULL;
        }
        header->owner.slab = NULL;
        return header + 1;
    }
    void **released = &pool->released[size_class - 1];
    if (*released) {
        void *storage = *released;
        memcpy(released, storage, sizeof(*released));
        return storage;
    }
    const size_t stride = sizeof(*header) + size_class * MARKDOWN_CORE_BYTES_CLASS;
    markdown_core_slab_pool *slabs = &pool->slabs;
    if ((!slabs->current || slab_bytes - sizeof(markdown_core_slab) - slabs->taken < stride) &&
        !markdown_core_slab_pool_grow(slabs, slab_bytes)) {
        return NULL;
    }
    header = (markdown_core_bytes_header *)((unsigned char *)(slabs->current + 1) + slabs->taken);
    slabs->taken += stride;
    header->owner.slab = slabs->current;
    header->owner.size_class = size_class;
    slabs->current->head.holds++;
    return header + 1;
}

/* Gives back storage `markdown_core_bytes_take` took: into the pool for
 * reuse, or, with no pool, dropping its slab hold. A slot from the allocator
 * is freed. */
static inline void markdown_core_bytes_release(markdown_core_bytes_pool *pool, void *storage) {
    markdown_core_bytes_header *header = (markdown_core_bytes_header *)storage - 1;
    if (!header->owner.slab) {
        markdown_core_free(header);
    } else if (pool) {
        void **released = &pool->released[header->owner.size_class - 1];
        memcpy(storage, released, sizeof(*released));
        *released = storage;
    } else {
        markdown_core_slab_drop(header->owner.slab);
    }
}

/* Bytes storage of the allocator's of `bytes`, holding what `storage`, the
 * allocator's bytes storage or NULL, held up to `bytes`: a buffer that grows
 * in place. NULL, `storage` kept, when it cannot be had. */
static inline void *markdown_core_bytes_resize(void *storage, size_t bytes) {
    markdown_core_bytes_header *header = storage ? (markdown_core_bytes_header *)storage - 1 : NULL;
    header = (markdown_core_bytes_header *)markdown_core_realloc(header, sizeof(*header) + bytes);
    if (!header) {
        return NULL;
    }
    header->owner.slab = NULL;
    return header + 1;
}

/* Drops what the pool holds: its released storage and its current slab. */
void markdown_core_bytes_pool_dispose(markdown_core_bytes_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
