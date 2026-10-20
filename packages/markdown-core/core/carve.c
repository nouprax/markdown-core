#include <stdint.h>
#include "alloc.h"
#include "carve.h"

#define CARVE_BLOCK_MIN_BYTES ((size_t)1024)
#define CARVE_BLOCK_MAX_BYTES ((size_t)16 * 1024)

void *markdown_core_carve_open(markdown_core_carve *carve, size_t size) {
    size_t capacity = carve->size ? carve->size * 2 : CARVE_BLOCK_MIN_BYTES;
    if (capacity > CARVE_BLOCK_MAX_BYTES) {
        capacity = CARVE_BLOCK_MAX_BYTES;
    }
    if (capacity < size) {
        capacity = size;
    }
    if (capacity > SIZE_MAX - sizeof(markdown_core_carve_block)) {
        return NULL;
    }
    markdown_core_carve_block *block =
        (markdown_core_carve_block *)markdown_core_realloc(NULL, sizeof(markdown_core_carve_block) + capacity);
    if (!block) {
        return NULL;
    }
    block->head.link.next = carve->blocks;
    block->head.link.size = capacity;
    carve->blocks = block;
    carve->size = capacity;
    carve->used = size;
    return block + 1;
}

void markdown_core_carve_rollback(markdown_core_carve *carve, markdown_core_carve mark) {
    while (carve->blocks != mark.blocks) {
        markdown_core_carve_block *block = carve->blocks;
        carve->blocks = block->head.link.next;
        markdown_core_free(block);
    }
    *carve = mark;
}

void markdown_core_carve_keep_oldest(markdown_core_carve *carve) {
    while (carve->blocks->head.link.next) {
        markdown_core_carve_block *block = carve->blocks;
        carve->blocks = block->head.link.next;
        markdown_core_free(block);
    }
    carve->used = 0;
    carve->size = carve->blocks->head.link.size;
}
