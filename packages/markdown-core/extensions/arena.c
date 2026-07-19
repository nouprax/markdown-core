#include "arena.h"

#include <stdint.h>
#include <string.h>

// Block layout. Every block hands out `usable = block + header` and stamps a
// tag word at usable[-8]: a class index for slab blocks, ARENA_LARGE_TAG for
// passthrough blocks. Slab blocks carry a 16-byte header (tag only); large
// blocks carry a 32-byte header adding the doubly-linked list pointers and
// the current usable capacity, so free and realloc never need a lookup.
//
//   slab block:   [unused 8][tag 8][usable ...]
//   large block:  [prev 8][next 8][capacity 8][tag 8][usable ...]
//
// Class sizes are powers of two from 32 to 2048 bytes including the header;
// 16-byte alignment holds throughout because slabs come from the base
// allocator and are carved in class-size steps.

#define ARENA_HEADER 16u
#define ARENA_LARGE_HEADER 32u
#define ARENA_CLASS_COUNT 7 /* 32, 64, 128, 256, 512, 1024, 2048 */
#define ARENA_CLASS_MAX (32u << (ARENA_CLASS_COUNT - 1))
#define ARENA_SLAB_BYTES (64u * 1024u)
#define ARENA_LARGE_TAG SIZE_MAX

typedef struct arena_slab {
    struct arena_slab *next;
} arena_slab;

typedef struct arena_free_block {
    struct arena_free_block *next;
} arena_free_block;

struct markdown_core_arena {
    markdown_core_mem mem; // must stay first: allocator calls cast back
    markdown_core_mem *base;
    arena_slab *slabs;
    unsigned char *carve; // next unassigned byte of the newest slab
    size_t remain;
    arena_free_block *frees[ARENA_CLASS_COUNT];
    unsigned char *larges; // newest passthrough block (block start)
};

static size_t class_bytes(size_t index) { return (size_t)32 << index; }

// Smallest class whose block holds `total` bytes header included;
// ARENA_CLASS_COUNT when only a passthrough block fits.
static size_t class_for(size_t total) {
    size_t index = 0;
    while (index < ARENA_CLASS_COUNT && class_bytes(index) < total) {
        index++;
    }
    return index;
}

static size_t *tag_of(void *usable) { return (size_t *)((unsigned char *)usable - 8); }

static unsigned char **large_prev(unsigned char *block) { return (unsigned char **)block; }
static unsigned char **large_next(unsigned char *block) { return (unsigned char **)(block + 8); }
static size_t *large_capacity(unsigned char *block) { return (size_t *)(block + 16); }

static void *large_alloc(markdown_core_arena *arena, size_t bytes, int zero) {
    unsigned char *block;
    if (bytes > SIZE_MAX - ARENA_LARGE_HEADER) {
        return NULL;
    }
    // Growth-driven blocks skip the calloc zeroing: repeatedly regrown
    // buffers (adoption stacks, content stores) would otherwise be wiped by
    // the base allocator on every round trip.
    block = zero ? (unsigned char *)arena->base->calloc(arena->base, 1, bytes + ARENA_LARGE_HEADER)
                 : (unsigned char *)arena->base->realloc(arena->base, NULL, bytes + ARENA_LARGE_HEADER);
    if (!block) {
        return NULL;
    }
    *large_prev(block) = NULL;
    *large_next(block) = arena->larges;
    if (arena->larges) {
        *large_prev(arena->larges) = block;
    }
    arena->larges = block;
    *large_capacity(block) = bytes;
    *tag_of(block + ARENA_LARGE_HEADER) = ARENA_LARGE_TAG;
    return block + ARENA_LARGE_HEADER;
}

static void large_unlink(markdown_core_arena *arena, unsigned char *block) {
    unsigned char *prev = *large_prev(block);
    unsigned char *next = *large_next(block);
    if (prev) {
        *large_next(prev) = next;
    } else {
        arena->larges = next;
    }
    if (next) {
        *large_prev(next) = prev;
    }
}

static void *arena_block(markdown_core_arena *arena, size_t bytes, int zero) {
    size_t index;
    unsigned char *block;
    void *usable;
    if (bytes > SIZE_MAX - ARENA_HEADER) {
        return NULL;
    }
    index = class_for(bytes + ARENA_HEADER);
    if (index >= ARENA_CLASS_COUNT) {
        return large_alloc(arena, bytes, zero);
    }
    if (arena->frees[index]) {
        block = (unsigned char *)arena->frees[index];
        arena->frees[index] = arena->frees[index]->next;
    } else {
        size_t need = class_bytes(index);
        if (arena->remain < need) {
            arena_slab *slab = (arena_slab *)arena->base->calloc(arena->base, 1, ARENA_SLAB_BYTES);
            if (!slab) {
                return NULL;
            }
            slab->next = arena->slabs;
            arena->slabs = slab;
            arena->carve = (unsigned char *)slab + ARENA_HEADER;
            arena->remain = ARENA_SLAB_BYTES - ARENA_HEADER;
        }
        block = arena->carve;
        arena->carve += need;
        arena->remain -= need;
        zero = 0; // slabs arrive zeroed and carved blocks are never dirty
    }
    usable = block + ARENA_HEADER;
    *tag_of(usable) = index;
    if (zero) {
        memset(usable, 0, bytes);
    }
    return usable;
}

static void *arena_calloc(markdown_core_mem *mem, size_t nmem, size_t size) {
    markdown_core_arena *arena = (markdown_core_arena *)mem;
    size_t bytes;
    if (nmem && size > SIZE_MAX / nmem) {
        return NULL;
    }
    bytes = nmem * size;
    return arena_block(arena, bytes ? bytes : 1, 1);
}

static void *arena_realloc(markdown_core_mem *mem, void *ptr, size_t size) {
    markdown_core_arena *arena = (markdown_core_arena *)mem;
    size_t tag;
    if (!ptr) {
        return arena_block(arena, size ? size : 1, 0);
    }
    tag = *tag_of(ptr);
    if (tag == ARENA_LARGE_TAG) {
        unsigned char *block = (unsigned char *)ptr - ARENA_LARGE_HEADER;
        unsigned char *grown;
        if (size <= *large_capacity(block)) {
            return ptr;
        }
        if (size > SIZE_MAX - ARENA_LARGE_HEADER) {
            return NULL;
        }
        large_unlink(arena, block);
        grown = (unsigned char *)arena->base->realloc(arena->base, block, size + ARENA_LARGE_HEADER);
        if (!grown) {
            // The old block is still valid; relink it so release finds it.
            *large_prev(block) = NULL;
            *large_next(block) = arena->larges;
            if (arena->larges) {
                *large_prev(arena->larges) = block;
            }
            arena->larges = block;
            return NULL;
        }
        *large_prev(grown) = NULL;
        *large_next(grown) = arena->larges;
        if (arena->larges) {
            *large_prev(arena->larges) = grown;
        }
        arena->larges = grown;
        *large_capacity(grown) = size;
        return grown + ARENA_LARGE_HEADER;
    }
    {
        size_t capacity = class_bytes(tag) - ARENA_HEADER;
        void *moved;
        if (size <= capacity) {
            return ptr;
        }
        moved = arena_block(arena, size, 0);
        if (!moved) {
            return NULL;
        }
        memcpy(moved, ptr, capacity);
        ((arena_free_block *)((unsigned char *)ptr - ARENA_HEADER))->next = arena->frees[tag];
        arena->frees[tag] = (arena_free_block *)((unsigned char *)ptr - ARENA_HEADER);
        return moved;
    }
}

static void arena_free(markdown_core_mem *mem, void *ptr) {
    markdown_core_arena *arena = (markdown_core_arena *)mem;
    size_t tag;
    if (!ptr) {
        return;
    }
    tag = *tag_of(ptr);
    if (tag == ARENA_LARGE_TAG) {
        unsigned char *block = (unsigned char *)ptr - ARENA_LARGE_HEADER;
        large_unlink(arena, block);
        arena->base->free(arena->base, block);
        return;
    }
    {
        arena_free_block *block = (arena_free_block *)((unsigned char *)ptr - ARENA_HEADER);
        block->next = arena->frees[tag];
        arena->frees[tag] = block;
    }
}

markdown_core_arena *markdown_core_arena_new(markdown_core_mem *base) {
    markdown_core_arena *arena = (markdown_core_arena *)base->calloc(base, 1, sizeof(*arena));
    if (!arena) {
        return NULL;
    }
    arena->mem.calloc = arena_calloc;
    arena->mem.realloc = arena_realloc;
    arena->mem.free = arena_free;
    arena->base = base;
    return arena;
}

markdown_core_mem *markdown_core_arena_mem(markdown_core_arena *arena) { return &arena->mem; }

void markdown_core_arena_release(markdown_core_arena *arena) {
    markdown_core_mem *base = arena->base;
    arena_slab *slab = arena->slabs;
    unsigned char *large = arena->larges;
    while (slab) {
        arena_slab *next = slab->next;
        base->free(base, slab);
        slab = next;
    }
    while (large) {
        unsigned char *next = *large_next(large);
        base->free(base, large);
        large = next;
    }
    base->free(base, arena);
}
