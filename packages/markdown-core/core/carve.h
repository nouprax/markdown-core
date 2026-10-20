#ifndef MARKDOWN_CORE_CARVE_H
#define MARKDOWN_CORE_CARVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RECORDS CARVED FROM BLOCKS THAT NEVER MOVE. A record lives until its store
 * is freed or a rollback pops it, and pops come newest first, so records are
 * carved one after another from blocks the store owns: a rollback releases
 * the blocks opened after its mark and resumes the mark's block where the
 * mark left it. A block starts small, so a parse that keeps a few records
 * costs one small allocation, and doubles up to a cap, so many records cost
 * a few allocations rather than one each. A record larger than the next
 * block gets a block of its own size. A record keeps its address for its
 * life, so its owner writes it in place whenever it likes, and no growth
 * copies it. */
typedef struct markdown_core_carve_block markdown_core_carve_block;

/* A block: the next older block and the bytes it has, then its records,
 * each aligned as the block is. */
struct markdown_core_carve_block {
    union {
        struct {
            markdown_core_carve_block *next;
            size_t size;
        } link;
        long double real;
        int64_t whole;
    } head;
};

typedef struct markdown_core_carve {
    /* The blocks, newest first, and the bytes of the newest that records
     * took and that it has. A mark is a copy of the store. */
    markdown_core_carve_block *blocks;
    size_t used, size;
} markdown_core_carve;

/* Opens a block for a record of `size` bytes, whole units of a block's
 * alignment, and carves the record from it; NULL when no block can be had. */
void *markdown_core_carve_open(markdown_core_carve *carve, size_t size);

/* Storage of `size` bytes, at least one, aligned for any value, or NULL when
 * none can be had: from the newest block while it has room. */
static inline void *markdown_core_carve_take(markdown_core_carve *carve, size_t size) {
    const size_t unit = sizeof(markdown_core_carve_block);
    if (size > SIZE_MAX - unit) {
        return NULL;
    }
    size = (size + unit - 1) / unit * unit;
    if (carve->size - carve->used < size) {
        return markdown_core_carve_open(carve, size);
    }
    void *storage = (unsigned char *)(carve->blocks + 1) + carve->used;
    carve->used += size;
    return storage;
}

/* Pops every record carved after `mark`, a copy the store was. */
void markdown_core_carve_rollback(markdown_core_carve *carve, markdown_core_carve mark);

/* Gives back every block but the oldest, and pops its records. */
void markdown_core_carve_keep_oldest(markdown_core_carve *carve);

/* Pops every record, keeping the first block for the records to come. */
static inline void markdown_core_carve_clear(markdown_core_carve *carve) {
    if (carve->blocks && carve->blocks->head.link.next) {
        markdown_core_carve_keep_oldest(carve);
    }
    carve->used = 0;
}

/* Gives back every block: pops every record, back to the empty store. */
static inline void markdown_core_carve_free(markdown_core_carve *carve) {
    if (carve->blocks) {
        markdown_core_carve_rollback(carve, (markdown_core_carve){0});
    }
}

#ifdef __cplusplus
}
#endif

#endif
