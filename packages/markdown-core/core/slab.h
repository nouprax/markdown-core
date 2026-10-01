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

/* FIXED-SIZE SLOTS, TAKEN FROM SLABS, for the objects a parse makes many of:
 * nodes, the runs of their children, the resources links read through and
 * the counts of the bytes literals read (node.h), and the parse's own inline
 * records (inlines.c).
 *
 * A slot is a header naming the slab it came from, then storage for one
 * object. A parse takes slots from SLABS -- one allocation of many slots --
 * and a caller with no parse takes one slot from the allocator; the header
 * says which, so an object is released the same way whichever storage it
 * came from, and nothing about the storage is visible through the object.
 *
 * Slots of every kind that live together are cut, one after another, from
 * the same slabs: a slot's size is its kind's, and a slab is cut until the
 * next slot does not fit. Each kind keeps a POOL of the slots released into
 * it, handed out again before another slot is cut, so a released slot is
 * reused by its own kind.
 *
 * A slab lives while anything holds it: every slot cut from it, and the
 * slabs while it is the one slots are cut from. A slot released into a pool
 * keeps its hold until the pool is disposed; a slot released with no pool
 * drops its hold, and the slab is freed with its last one -- by whichever
 * release that turns out to be. Disposing the pools and the slabs drops only
 * the holds they have, so what the parse built outlives the parse and keeps
 * its slabs. Slots of one slab are released from one thread at a time; two
 * parses never share a slab.
 *
 * A kind's slot size is a constant its code passes to every call, so the
 * arithmetic folds at each call, and the slabs and the pools are only their
 * state, which starts zeroed. */

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

/* The slabs slots are cut from. */
typedef struct markdown_core_slabs {
    /* The slab slots are being cut from, held by the slabs. */
    markdown_core_slab *current;
    /* Where its next slot begins, and its end. */
    unsigned char *next, *end;
} markdown_core_slabs;

/* One kind's released slots, linked through their storage's first bytes. */
typedef struct markdown_core_slab_pool {
    void *released;
} markdown_core_slab_pool;

/* The size of every slab. */
#define MARKDOWN_CORE_SLAB_BYTES ((size_t)64 * 1024)

/* How far apart a slab's slots of `bytes` of storage are. */
#define MARKDOWN_CORE_SLOT_STRIDE(bytes)                                                                               \
    (sizeof(markdown_core_slot_header) + ((bytes) + sizeof(markdown_core_slot_header) - 1) /                           \
                                             sizeof(markdown_core_slot_header) * sizeof(markdown_core_slot_header))

/* Starts a slab for the slabs to cut from, dropping their hold on the one it
 * replaces. False, leaving the slabs as they were, when the slab cannot be
 * allocated. */
bool markdown_core_slabs_grow(markdown_core_slabs *slabs);

/* Drops the slabs' hold on the slab slots are cut from. Slots cut from it
 * keep it alive after this. */
void markdown_core_slabs_dispose(markdown_core_slabs *slabs);

/* Drops the holds of the pool's released slots. */
void markdown_core_slab_pool_dispose(markdown_core_slab_pool *pool);

/* WHERE A NODE'S STORAGE COMES FROM, and where it goes back to.
 *
 * A node lives in a SLOT holding the node and room for its kind's
 * record, and the runs of its children, the resources links read through and
 * the counts of the bytes literals read live in slots of their own, all cut
 * from the pool's slabs. A parse takes every slot from the pool its
 * caller lends it -- a session's, which outlives each of its edits, or one
 * the caller makes for a single parse -- and a caller with no pool takes one
 * slot from the allocator. A slot released into a pool goes back to it for
 * reuse, so a session's edits reuse the slots of the nodes they retire
 * instead of pinning a slab per edit; a slot released with no pool drops its
 * slab hold. So a subtree taken from a parsed document is as good as one
 * built by hand: it outlives the pool it came from and is released by
 * `markdown_core_node_free` like any other. The size of what it keeps alive
 * is the slab, not the node.
 *
 * Why slabs: a node's chunk was larger than the C library's fast-path size
 * classes, so every release of one walked the allocator's merge path, and the
 * document's teardown cost more than a third of its parse. */
typedef struct markdown_core_node_pool {
    /* The slabs every slot below is cut from. */
    markdown_core_slabs slabs;
    markdown_core_slab_pool nodes;
    markdown_core_slab_pool resources;
    /* The runs of children trees (children.h). */
    markdown_core_slab_pool runs;
    /* The holders' counts of the bytes literals read. */
    markdown_core_slab_pool bytes;
} markdown_core_node_pool;

static inline void markdown_core_slab_drop(markdown_core_slab *slab) {
    if (slab && --slab->head.holds == 0) {
        markdown_core_free(slab);
    }
}

/* Uninitialized storage for one object of `bytes`, from the pool's released
 * slots, else cut from `slabs`; or one slot from the allocator when there are
 * no slabs (and no pool). NULL when none can be had. */
static inline void *markdown_core_slab_take(markdown_core_slabs *slabs, markdown_core_slab_pool *pool, size_t bytes) {
    markdown_core_slot_header *slot;
    if (!slabs) {
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
    if ((size_t)(slabs->end - slabs->next) < MARKDOWN_CORE_SLOT_STRIDE(bytes) && !markdown_core_slabs_grow(slabs)) {
        return NULL;
    }
    slot = (markdown_core_slot_header *)slabs->next;
    slabs->next += MARKDOWN_CORE_SLOT_STRIDE(bytes);
    slot->slab = slabs->current;
    slabs->current->head.holds++;
    return slot + 1;
}

/* Puts the slot whose storage is `storage`, taken from `pool`, back into
 * `pool` for reuse: a slot a pool hands out is always a slab's. */
static inline void markdown_core_slab_return(markdown_core_slab_pool *pool, void *storage) {
    memcpy(storage, &pool->released, sizeof(pool->released));
    pool->released = storage;
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
