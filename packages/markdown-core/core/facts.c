#include <string.h>

#include "alloc.h"
#include "facts.h"

markdown_core_fact *markdown_core_fact_new(markdown_core_fact_kind kind) {
    markdown_core_fact *fact = markdown_core_alloc(1, sizeof(*fact));
    if (fact) {
        fact->refs = 1;
        fact->kind = (uint8_t)kind;
    }
    return fact;
}

markdown_core_node *markdown_core_registry_new(markdown_core_node_pool *pool) {
    return markdown_core_node_pool_new(pool, MARKDOWN_CORE_NODE_REGISTRY, NULL);
}

markdown_core_node *markdown_core_registry_place(markdown_core_node_pool *pool, markdown_core_fact *fact,
                                                 int64_t lead) {
    markdown_core_node *entry = markdown_core_node_pool_new(pool, MARKDOWN_CORE_NODE_FACT, NULL);
    if (entry) {
        entry->as.fact_place->fact = markdown_core_fact_retain(fact);
        entry->where.extent = (markdown_core_extent){(int32_t)lead, 0};
    }
    return entry;
}

size_t markdown_core_registry_seek(const markdown_core_node *registry, int64_t position) {
    size_t index;
    int64_t lead;
    /* An entry ends where its fact is, so the first one ending after the
     * byte before `position` is the first at or after it. */
    return markdown_core_children_find(registry->children, 0, position - 1, &index, &lead)
               ? index
               : markdown_core_registry_count(registry);
}

int64_t markdown_core_registry_position(const markdown_core_node *registry, size_t index) {
    const markdown_core_run *run = registry->children;
    int64_t position = 0;
    while (run->tier) {
        size_t k = 0;
        for (;; k++) {
            const markdown_core_run *entry = run->entries[k];
            if (index < entry->total) {
                break;
            }
            index -= entry->total;
            position += entry->length;
        }
        run = run->entries[k];
    }
    for (size_t k = 0; k <= index; k++) {
        position += ((const markdown_core_node *)run->entries[k])->where.extent.lead;
    }
    return position;
}

/* The last fact under `run`. */
static const markdown_core_fact *S_last(const markdown_core_run *run) {
    while (run->tier) {
        run = run->entries[run->count - 1];
    }
    return markdown_core_fact_of(run->entries[run->count - 1]);
}

size_t markdown_core_registry_search(const markdown_core_node *registry,
                                     bool (*below)(const markdown_core_fact *fact, const void *key), const void *key) {
    const markdown_core_run *run = registry->children;
    size_t index = 0;
    if (!run) {
        return 0;
    }
    /* Down the first entry whose last fact `below` does not hold for. */
    while (run->tier) {
        size_t k = 0;
        while (k < run->count && below(S_last(run->entries[k]), key)) {
            index += ((const markdown_core_run *)run->entries[k])->total;
            k++;
        }
        if (k == run->count) {
            return index;
        }
        run = run->entries[k];
    }
    size_t k = 0;
    while (k < run->count && below(markdown_core_fact_of(run->entries[k]), key)) {
        k++;
    }
    return index + k;
}

static bool S_before(const markdown_core_fact *fact, const void *key) {
    return fact->order < ((const markdown_core_fact *)key)->order;
}

size_t markdown_core_registry_rank(const markdown_core_node *registry, const markdown_core_fact *fact) {
    return markdown_core_registry_search(registry, S_before, fact);
}

bool markdown_core_registry_build_begin(markdown_core_registry_builder *builder, markdown_core_node_pool *pool) {
    *builder = (markdown_core_registry_builder){pool, markdown_core_registry_new(pool), 0, NULL, 0, 0};
    return builder->registry != NULL;
}

bool markdown_core_registry_build_put(markdown_core_registry_builder *builder, markdown_core_fact *fact,
                                      int64_t position) {
    size_t index = markdown_core_registry_count(builder->registry);
    if (builder->fresh_count &&
        builder->fresh[builder->fresh_count - 1].first + builder->fresh[builder->fresh_count - 1].count == index) {
        builder->fresh[builder->fresh_count - 1].count++;
    } else {
        void *grown = markdown_core_reserve(builder->fresh, &builder->fresh_capacity, builder->fresh_count + 1,
                                            sizeof(*builder->fresh));
        if (!grown) {
            return false;
        }
        builder->fresh = grown;
        builder->fresh[builder->fresh_count].first = index;
        builder->fresh[builder->fresh_count].count = 1;
        builder->fresh_count++;
    }
    markdown_core_node *entry = markdown_core_registry_place(builder->pool, fact, position - builder->end);
    if (!entry || !markdown_core_children_append(builder->pool, &builder->registry->children, entry)) {
        if (entry) {
            markdown_core_node_pool_release(builder->pool, entry);
        }
        builder->fresh[builder->fresh_count - 1].count--;
        return false;
    }
    builder->end = position;
    return true;
}

bool markdown_core_registry_build_join(markdown_core_registry_builder *builder, const markdown_core_node *old,
                                       size_t first, size_t count, int64_t position) {
    if (!count) {
        return true;
    }
    int64_t span =
        markdown_core_registry_position(old, first + count - 1) - markdown_core_registry_position(old, first);
    bool ok = true;
    markdown_core_run *run = markdown_core_children_slice(builder->pool, old->children, first, count, &ok);
    if (!ok) {
        return false;
    }
    /* The run's first fact follows another fact here than in `old`. */
    markdown_core_node *head = markdown_core_children_at(run, 0);
    if (head->where.extent.lead != position - builder->end) {
        markdown_core_node *entry =
            markdown_core_registry_place(builder->pool, markdown_core_fact_of(head), position - builder->end);
        markdown_core_node *replaced;
        if (!entry || !markdown_core_children_replace(builder->pool, &run, 0, entry, &replaced)) {
            if (entry) {
                markdown_core_node_pool_release(builder->pool, entry);
            }
            markdown_core_node_pool_release_children(builder->pool, run);
            return false;
        }
        markdown_core_node_pool_release(builder->pool, replaced);
    }
    if (!markdown_core_children_join(builder->pool, &builder->registry->children, run)) {
        return false;
    }
    builder->end = position + span;
    return true;
}

markdown_core_node *markdown_core_registry_build_end(markdown_core_registry_builder *builder, bool ok) {
    if (!ok) {
        markdown_core_node_pool_release(builder->pool, builder->registry);
        markdown_core_registry_build_cancel(builder);
        return NULL;
    }
    markdown_core_children_seal(builder->registry->children);
    return builder->registry;
}

/* The order of the fact at `index`. */
static uint64_t S_order_at(const markdown_core_node *registry, size_t index) {
    return markdown_core_fact_of(markdown_core_children_at(registry->children, index))->order;
}

/* Orders the facts [first, first + count) of `registry`, which have none,
 * and any with none right after them. The window [low, high) of facts to
 * number grows on both sides until the orders around it leave more room
 * than the square of the facts it holds; they are then spread evenly over
 * it. At the end of the registry, where streaming appends, the spacing
 * stops at 2^32, so appends do not halve the room left each time. */
static void S_order_run(markdown_core_node *registry, size_t first, size_t count) {
    size_t total = markdown_core_registry_count(registry), low = first, high = first + count;
    for (;;) {
        while (high < total && !S_order_at(registry, high)) {
            high++;
        }
        uint64_t below = low ? S_order_at(registry, low - 1) : 0;
        uint64_t above = high < total ? S_order_at(registry, high) : UINT64_MAX;
        uint64_t width = (uint64_t)(high - low) + 1;
        if (above - below > width * width) {
            uint64_t spacing = (above - below) / width;
            if (high == total && spacing > (UINT64_C(1) << 32)) {
                spacing = UINT64_C(1) << 32;
            }
            markdown_core_children_cursor cursor;
            markdown_core_children_seek(&cursor, registry->children, low);
            for (uint64_t k = 1; k < width; k++) {
                markdown_core_fact_of(markdown_core_children_next(&cursor))->order = below + spacing * k;
            }
            return;
        }
        size_t grow = high - low;
        high = total - high < grow ? total : high + grow;
        low = low < grow ? 0 : low - grow;
    }
}

void markdown_core_registry_build_order(markdown_core_registry_builder *builder) {
    for (size_t i = 0; i < builder->fresh_count; i++) {
        if (builder->fresh[i].count && !S_order_at(builder->registry, builder->fresh[i].first)) {
            S_order_run(builder->registry, builder->fresh[i].first, builder->fresh[i].count);
        }
    }
    markdown_core_registry_build_cancel(builder);
}

void markdown_core_registry_build_cancel(markdown_core_registry_builder *builder) {
    markdown_core_free(builder->fresh);
    builder->fresh = NULL;
    builder->fresh_count = builder->fresh_capacity = 0;
}
