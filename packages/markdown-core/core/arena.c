#include <stdint.h>
#include <string.h>

#include "arena.h"

/* Every record is aligned for any scalar, which is also the size granule of
 * the recycling pools; `pool_class` below assigns a record of `size` bytes to
 * its class. */
#define ARENA_GRANULE MARKDOWN_CORE_ARENA_GRANULE
#define ARENA_EXACT_CLASSES MARKDOWN_CORE_ARENA_CLASSES
/* One class per power of two above the exact ones; a size_t has no more. */
#define ARENA_WIDE_CLASSES (sizeof(size_t) * 8)
#define ARENA_CLASSES (ARENA_EXACT_CLASSES + ARENA_WIDE_CLASSES)
#define ARENA_UNPOOLED ((size_t)-1)
#define ARENA_FIRST_BLOCK 4096
#define ARENA_TEXT_SLAB 256
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
    /* The slab the next unaligned text ask is cut from, and what is left of
     * it. A count rather than an end pointer: an arena that has served no
     * text yet has no slab, and there is no pointer to compare against. */
    unsigned char *text_at;
    size_t text_left;
    free_record *pools[ARENA_CLASSES];
};

static size_t round_up(size_t size) { return (size + (ARENA_GRANULE - 1)) & ~(size_t)(ARENA_GRANULE - 1); }

static unsigned char *block_data(arena_block *block) { return (unsigned char *)block + round_up(sizeof(*block)); }

/* The arena record and its first block share one allocation: the block
 * follows the record at the next granule. Nothing is asked to zero the
 * block's data; a record is cleared when it is taken. */
static arena_block *first_block(markdown_core_arena *arena) {
    return (arena_block *)((unsigned char *)arena + round_up(sizeof(*arena)));
}

markdown_core_arena *markdown_core_arena_new(markdown_core_mem *mem) {
    size_t header = round_up(sizeof(arena_block));
    markdown_core_arena *arena = mem->realloc(NULL, round_up(sizeof(*arena)) + header + ARENA_FIRST_BLOCK);
    if (!arena) {
        return NULL;
    }
    memset(arena, 0, sizeof(*arena));
    arena->mem = mem;
    arena_block *block = first_block(arena);
    block->next = NULL;
    block->used = 0;
    block->capacity = ARENA_FIRST_BLOCK;
    arena->current = block;
    arena->next_capacity = 2 * ARENA_FIRST_BLOCK;
    return arena;
}

void markdown_core_arena_free(markdown_core_arena *arena) {
    if (!arena) {
        return;
    }
    markdown_core_mem *mem = arena->mem;
    arena_block *embedded = first_block(arena);
    arena_block *block = arena->current;
    while (block) {
        arena_block *next = block->next;
        if (block != embedded) {
            mem->free(block);
        }
        block = next;
    }
    mem->free(arena);
}

markdown_core_mem *markdown_core_arena_mem(const markdown_core_arena *arena) { return arena->mem; }

/* Blocks double up to a bound, so the space held beyond the live records is
 * at most one block, and a request larger than that bound gets its own. A
 * block is not zeroed: `markdown_core_arena_alloc` hands out bytes its
 * caller writes in full, and a taken record is cleared on the spot. */
static arena_block *arena_grow(markdown_core_arena *arena, size_t needed) {
    size_t capacity = arena->next_capacity;
    if (capacity < needed) {
        capacity = needed;
    }
    size_t header = round_up(sizeof(arena_block));
    if (capacity > SIZE_MAX - header) {
        return NULL;
    }
    arena_block *block = arena->mem->realloc(NULL, header + capacity);
    if (!block) {
        return NULL;
    }
    block->used = 0;
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

/* Storage for bytes, which need no alignment: the copied text of a value, a
 * name, a destination. A slab of records serves many of them, so a document's
 * short strings cost their length rather than a granule each, and the record
 * path above is untouched -- a parse that copies no text pays nothing. */
void *markdown_core_arena_text(markdown_core_arena *arena, size_t size) {
    if (!size) {
        size = 1;
    }
    if (arena->text_left < size) {
        size_t slab = size > ARENA_TEXT_SLAB ? size : ARENA_TEXT_SLAB;
        unsigned char *fresh = markdown_core_arena_alloc(arena, slab);
        if (!fresh) {
            return NULL;
        }
        arena->text_at = fresh;
        arena->text_left = round_up(slab);
    }
    unsigned char *record = arena->text_at;
    arena->text_at += size;
    arena->text_left -= size;
    return record;
}

bool markdown_core_arena_extend(markdown_core_arena *arena, const void *storage, size_t size, size_t needed) {
    arena_block *block = arena->current;
    if (!block || needed < size || needed > SIZE_MAX - ARENA_GRANULE) {
        return false;
    }
    size_t held = round_up(size ? size : 1), wanted = round_up(needed);
    if ((const unsigned char *)storage + held != block_data(block) + block->used ||
        wanted - held > block->capacity - block->used) {
        return false;
    }
    block->used += wanted - held;
    return true;
}

/* A class holds records of exactly one size, so a take can only ever receive
 * a record as large as it asked for. Every size up to the granule ceiling has
 * a class of its own; above it, the powers of two do, which is the shape a
 * doubling vector grows through -- the storage each growth supersedes goes
 * back to the pool instead of lying dead in the arena for the document's
 * life. An oversized record of any other size has no class and is released
 * with the arena, as every oversized record used to be. */
static size_t pool_class(size_t size) {
    size_t units = round_up(size ? size : 1) / ARENA_GRANULE;
    if (units <= ARENA_EXACT_CLASSES) {
        return units - 1;
    }
    if (units & (units - 1)) {
        return ARENA_UNPOOLED;
    }
    size_t wide = 0;
    while (((size_t)1 << wide) != units) {
        wide++;
    }
    return ARENA_EXACT_CLASSES + wide;
}

void *markdown_core_arena_take(markdown_core_arena *arena, size_t size) {
    size_t class = pool_class(size);
    if (class != ARENA_UNPOOLED && arena->pools[class]) {
        free_record *record = arena->pools[class];
        arena->pools[class] = record->next;
        memset(record, 0, round_up(size ? size : 1));
        return record;
    }
    void *record = markdown_core_arena_alloc(arena, size);
    if (record) {
        memset(record, 0, round_up(size ? size : 1));
    }
    return record;
}

void markdown_core_arena_recycle(markdown_core_arena *arena, void *record, size_t size) {
    size_t class = pool_class(size);
    if (!record || class == ARENA_UNPOOLED) {
        return;
    }
    free_record *entry = record;
    entry->next = arena->pools[class];
    arena->pools[class] = entry;
}

bool markdown_core_arena_owns(const markdown_core_arena *arena, const void *record) {
    /* On the addresses as integers: a relational comparison of pointers into
     * different objects is undefined, and a record of another transaction is
     * exactly such a pointer. The unsigned difference wraps for an address
     * below the block, so one test covers both ends of the block. */
    uintptr_t at = (uintptr_t)record;
    for (const arena_block *block = arena->current; block; block = block->next) {
        uintptr_t data = (uintptr_t)block_data((arena_block *)block);
        if (at - data < (uintptr_t)block->used) {
            return true;
        }
    }
    return false;
}
