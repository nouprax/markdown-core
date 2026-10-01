#include "slab.h"

bool markdown_core_slabs_grow(markdown_core_slabs *slabs) {
    markdown_core_slab *slab = (markdown_core_slab *)markdown_core_realloc(NULL, MARKDOWN_CORE_SLAB_BYTES);
    if (!slab) {
        return false;
    }
    slab->head.holds = 1;
    markdown_core_slab_drop(slabs->current);
    slabs->current = slab;
    slabs->next = (unsigned char *)(slab + 1);
    slabs->end = (unsigned char *)slab + MARKDOWN_CORE_SLAB_BYTES;
    return true;
}

void markdown_core_slabs_dispose(markdown_core_slabs *slabs) {
    markdown_core_slab_drop(slabs->current);
    *slabs = (markdown_core_slabs){0};
}

void markdown_core_slab_pool_dispose(markdown_core_slab_pool *pool) {
    while (pool->released) {
        void *storage = pool->released;
        memcpy(&pool->released, storage, sizeof(pool->released));
        markdown_core_slab_drop(((markdown_core_slot_header *)storage - 1)->slab);
    }
}
