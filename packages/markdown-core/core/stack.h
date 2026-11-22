#ifndef MARKDOWN_CORE_STACK_H
#define MARKDOWN_CORE_STACK_H
#include <stddef.h>
#include <string.h>
#include "alloc.h"

/* THE ENGINE'S STACK (docs/plans no-flush.md, the stack item of the
 * audit): one class for every list the inline parse keeps of what it read,
 * whose instances are the run's delimiters, its open brackets, its
 * suspended brackets, and the tokens an element reads in a body. An item
 * is a struct whose first member is a `markdown_core_stack_item`; the
 * stack links its items in the order they were pushed, `first` the oldest
 * and `last` the newest, and any of them leaves it in constant time.
 *
 * Items come from a POOL, which keeps the items its stacks let go of and
 * hands them out again, so the allocator is asked only when more items of
 * its kind are live at once than ever were before; it is released with its
 * owner. A pool takes items of one size. */

typedef struct markdown_core_stack_item {
    struct markdown_core_stack_item *previous, *next;
} markdown_core_stack_item;

typedef struct markdown_core_stack {
    markdown_core_stack_item *first, *last;
} markdown_core_stack;

typedef struct markdown_core_pool {
    markdown_core_stack_item *free;
} markdown_core_pool;

/* `item` goes on the stack, after its last. */
static inline void markdown_core_stack_push(markdown_core_stack *stack, markdown_core_stack_item *item) {
    item->previous = stack->last;
    item->next = NULL;
    if (stack->last) {
        stack->last->next = item;
    } else {
        stack->first = item;
    }
    stack->last = item;
}

/* `item`, on the stack, leaves it. */
static inline void markdown_core_stack_unlink(markdown_core_stack *stack, markdown_core_stack_item *item) {
    if (item->next) {
        item->next->previous = item->previous;
    } else {
        stack->last = item->previous;
    }
    if (item->previous) {
        item->previous->next = item->next;
    } else {
        stack->first = item->next;
    }
}

/* An item of `size` bytes, zeroed, from the pool, or NULL when storage could
 * not be had. */
static inline void *markdown_core_pool_take(markdown_core_pool *pool, size_t size) {
    markdown_core_stack_item *item = pool->free;
    if (!item) {
        return markdown_core_alloc(1, size);
    }
    pool->free = item->next;
    memset(item, 0, size);
    return item;
}

/* `item`, on no stack, goes back to the pool. */
static inline void markdown_core_pool_put(markdown_core_pool *pool, markdown_core_stack_item *item) {
    item->next = pool->free;
    pool->free = item;
}

/* Every item the pool keeps goes back to the allocator. */
static inline void markdown_core_pool_release(markdown_core_pool *pool) {
    while (pool->free) {
        markdown_core_stack_item *next = pool->free->next;
        markdown_core_free(pool->free);
        pool->free = next;
    }
}

/* A stack of items of `type`, whose first member is its `item`: its typed
 * reads, `prefix`_first, _last, _previous and _next, each NULL past the
 * stack's end. */
#define MARKDOWN_CORE_STACK_OF(prefix, type)                                                                           \
    static inline type *prefix##_first(const markdown_core_stack *stack) { return (type *)(void *)stack->first; }      \
    static inline type *prefix##_last(const markdown_core_stack *stack) { return (type *)(void *)stack->last; }        \
    static inline type *prefix##_previous(const type *item) { return (type *)(void *)item->item.previous; }            \
    static inline type *prefix##_next(const type *item) { return (type *)(void *)item->item.next; }

#endif
