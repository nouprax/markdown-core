#include "slab.h"

bool markdown_core_slab_pool_grow(markdown_core_slab_pool *pool, size_t slab_bytes, size_t stride) {
    markdown_core_slab *slab = (markdown_core_slab *)markdown_core_realloc(NULL, slab_bytes);
    if (!slab) {
        return false;
    }
    slab->head.holds = 1;
    markdown_core_slab_drop(pool->current);
    pool->current = slab;
    pool->next = (unsigned char *)(slab + 1);
    pool->end = pool->next + (slab_bytes - sizeof(*slab)) / stride * stride;
    return true;
}

void markdown_core_slab_pool_dispose(markdown_core_slab_pool *pool) {
    while (pool->released) {
        void *storage = pool->released;
        memcpy(&pool->released, storage, sizeof(pool->released));
        markdown_core_slab_drop(((markdown_core_slot_header *)storage - 1)->slab);
    }
    markdown_core_slab_drop(pool->current);
    pool->current = NULL;
    pool->next = pool->end = NULL;
}
