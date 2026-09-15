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

/* Carve `rounded` bytes off the current block, growing the arena when it does
 * not hold them. `rounded` is a granule multiple and non-zero: each caller
 * below has established that, and this is the whole of what the two of them
 * share.
 *
 * A take carves through here rather than through the exported entry point
 * because that one is emitted out of line whatever the compiler decides about
 * folding it into a caller, and what it decides moves with the rest of the
 * file: it stopped folding it into `markdown_core_arena_take` when this file
 * gained a second caller of it in `markdown_core_arena_text`. Carving through
 * a static function makes that decision stop reaching the take path. */
static void *arena_carve(markdown_core_arena *arena, size_t rounded) {
    arena_block *block = arena->current;
    if (!block || block->capacity - block->used < rounded) {
        block = arena_grow(arena, rounded);
        if (!block) {
            return NULL;
        }
    }
    void *record = block_data(block) + block->used;
    block->used += rounded;
    return record;
}

void *markdown_core_arena_alloc(markdown_core_arena *arena, size_t size) {
    if (size > SIZE_MAX - ARENA_GRANULE) {
        return NULL;
    }
    return arena_carve(arena, round_up(size ? size : 1));
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

/* The class that holds a record of `size`, and the size that class holds.
 * These are the pools' shape, read in the two directions the arena needs: a
 * take asks which class an ask belongs to, and everything after asks how much
 * a record of that class is. Neither restates the other -- `class_size` is
 * the only place a class's size is written.
 *
 * Every size up to the granule ceiling has a class of its own; above the
 * ceiling the powers of two do, which is the shape a record that grows by
 * doubling asks for, and a larger ask of any other size is served by the next
 * power.
 *
 * Rounding here rather than leaving such a size unpooled is what makes the
 * contract total: every record this arena hands out can be handed back,
 * whatever its size, so a caller that means to replace its storage never has
 * to know which sizes the pools happen to accept -- and a size that drifts
 * past the ceiling cannot silently stop being recycled. Only a record above
 * the ceiling pays the round-up, and only to the next power of two.
 *
 * ARENA_NO_CLASS means no pool can serve the ask: a size within a granule of
 * the end of a size_t carries the round-up past the end and wraps, and one
 * above the largest representable power of two is never reached by doubling.
 * Neither names storage that exists, so a take reports the allocation failure
 * markdown_core_arena_alloc reports for the same size, and a recycle -- which
 * could only have been handed such a size by a record that was never served
 * -- does nothing. */
#define ARENA_NO_CLASS SIZE_MAX

/* The powers of two above the ceiling, kept out of line: a record of a fixed
 * size never reaches them, and a loop in the middle of the class lookup would
 * sit on the path of every take that does not. */
static size_t pool_class_above_ceiling(size_t want) {
    if (!want) {
        return ARENA_NO_CLASS;
    }
    size_t power = (size_t)MARKDOWN_CORE_ARENA_RECYCLED_MAX, wide = 0;
    while (power < want) {
        if (power > SIZE_MAX / 2) {
            return ARENA_NO_CLASS;
        }
        power <<= 1;
        wide++;
    }
    return ARENA_EXACT_CLASSES + wide;
}

static size_t pool_class_for(size_t size) {
    size_t want = round_up(size ? size : 1);
    /* One comparison admits the exact classes and rejects a wrapped round-up
     * with them: zero minus one is the largest size_t, so it lands where the
     * sizes above the ceiling are, and that is where it is refused. */
    if (want - 1 < (size_t)MARKDOWN_CORE_ARENA_RECYCLED_MAX) {
        return want / ARENA_GRANULE - 1;
    }
    return pool_class_above_ceiling(want);
}

/* An exact class holds as many granules as its position; a wide one holds the
 * ceiling doubled as many times as its position is above the exact classes. */
static size_t class_size(size_t class) {
    if (class < ARENA_EXACT_CLASSES) {
        return (class + 1) * (size_t)ARENA_GRANULE;
    }
    return (size_t)MARKDOWN_CORE_ARENA_RECYCLED_MAX << (class - ARENA_EXACT_CLASSES);
}

void *markdown_core_arena_take(markdown_core_arena *arena, size_t size) {
    size_t class = pool_class_for(size);
    if (class == ARENA_NO_CLASS) {
        return NULL;
    }
    /* A class's records and a freshly carved one are the same storage at the
     * same size; only where they come from differs, so one clear serves both.
     * A class's size is already a granule multiple, so the carve rounds
     * nothing. */
    free_record *record = arena->pools[class];
    if (record) {
        arena->pools[class] = record->next;
    } else {
        record = arena_carve(arena, class_size(class));
        if (!record) {
            return NULL;
        }
    }
    memset(record, 0, class_size(class));
    return record;
}

void markdown_core_arena_recycle(markdown_core_arena *arena, void *record, size_t size) {
    size_t class = pool_class_for(size);
    if (!record || class == ARENA_NO_CLASS) {
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
