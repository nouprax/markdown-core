#include <stdint.h>
#include <string.h>

#include "arena.h"

/* Every record is aligned for any scalar, which is also the size granule of
 * the recycling pools: a record of `size` bytes belongs to class
 * ceil(size / GRANULE) - 1. Larger records are never recycled. */
#define ARENA_GRANULE 16
#define ARENA_CLASSES 64
#define ARENA_FIRST_BLOCK 4096
#define ARENA_LARGEST_BLOCK (256u << 10)

typedef struct arena_block {
    struct arena_block *next;
    size_t used, capacity;
    /* Data follows at the next multiple of ARENA_GRANULE. */
} arena_block;

typedef struct free_record {
    struct free_record *next;
} free_record;

struct markdown_core_arena {
    markdown_core_mem *mem;
    arena_block *current;
    size_t next_capacity;
    free_record *pools[ARENA_CLASSES];
};

static size_t round_up(size_t size) { return (size + (ARENA_GRANULE - 1)) & ~(size_t)(ARENA_GRANULE - 1); }

static unsigned char *block_data(arena_block *block) { return (unsigned char *)block + round_up(sizeof(*block)); }

markdown_core_arena *markdown_core_arena_new(markdown_core_mem *mem) {
    markdown_core_arena *arena = mem->calloc(1, sizeof(*arena));
    if (!arena) {
        return NULL;
    }
    arena->mem = mem;
    arena->next_capacity = ARENA_FIRST_BLOCK;
    return arena;
}

void markdown_core_arena_free(markdown_core_arena *arena) {
    if (!arena) {
        return;
    }
    markdown_core_mem *mem = arena->mem;
    arena_block *block = arena->current;
    while (block) {
        arena_block *next = block->next;
        mem->free(block);
        block = next;
    }
    mem->free(arena);
}

markdown_core_mem *markdown_core_arena_mem(const markdown_core_arena *arena) { return arena->mem; }

/* Blocks double up to a bound, so the space held beyond the live records is
 * at most one block, and a request larger than that bound gets its own. */
static arena_block *arena_grow(markdown_core_arena *arena, size_t needed) {
    size_t capacity = arena->next_capacity;
    if (capacity < needed) {
        capacity = needed;
    }
    size_t header = round_up(sizeof(arena_block));
    if (capacity > SIZE_MAX - header) {
        return NULL;
    }
    arena_block *block = arena->mem->calloc(1, header + capacity);
    if (!block) {
        return NULL;
    }
    block->capacity = capacity;
    block->next = arena->current;
    arena->current = block;
    if (needed <= arena->next_capacity && arena->next_capacity < ARENA_LARGEST_BLOCK) {
        arena->next_capacity *= 2;
    }
    return block;
}

void *markdown_core_arena_alloc(markdown_core_arena *arena, size_t size) {
    if (size > SIZE_MAX - ARENA_GRANULE) {
        return NULL;
    }
    size = round_up(size ? size : 1);
    arena_block *block = arena->current;
    if (!block || block->capacity - block->used < size) {
        block = arena_grow(arena, size);
        if (!block) {
            return NULL;
        }
    }
    void *record = block_data(block) + block->used;
    block->used += size;
    return record;
}

static size_t pool_class(size_t size) { return (round_up(size ? size : 1) / ARENA_GRANULE) - 1; }

void *markdown_core_arena_take(markdown_core_arena *arena, size_t size) {
    size_t class = pool_class(size);
    if (class < ARENA_CLASSES && arena->pools[class]) {
        free_record *record = arena->pools[class];
        arena->pools[class] = record->next;
        memset(record, 0, (class + 1) * ARENA_GRANULE);
        return record;
    }
    return markdown_core_arena_alloc(arena, size);
}

void markdown_core_arena_recycle(markdown_core_arena *arena, void *record, size_t size) {
    size_t class = pool_class(size);
    if (!record || class >= ARENA_CLASSES) {
        return;
    }
    free_record *entry = record;
    entry->next = arena->pools[class];
    arena->pools[class] = entry;
}

bool markdown_core_arena_owns(const markdown_core_arena *arena, const void *record) {
    const unsigned char *at = (const unsigned char *)record;
    for (const arena_block *block = arena->current; block; block = block->next) {
        const unsigned char *data = block_data((arena_block *)block);
        if (at >= data && at < data + block->used) {
            return true;
        }
    }
    return false;
}
