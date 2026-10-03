#ifndef MARKDOWN_CORE_CHILDREN_H
#define MARKDOWN_CORE_CHILDREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "slab.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_node;

/* A NODE'S CHILDREN, as a balanced tree of shared runs
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.1 and 5.11).
 *
 * A run holds up to MARKDOWN_CORE_RUN_WIDTH entries: nodes when its tier is
 * zero, and runs of the tier below otherwise. Every run but the last of its
 * tier -- the runs of the tree's end, the edge an open block grows along --
 * holds at least half the width, every run holds an entry, and every
 * tier-zero run is at the same depth, so finding, inserting and removing a
 * child costs O(log children). A tree grows at its end the way a builder
 * fills it: a child appended to a full last run starts the next run and
 * leaves the full one as it is, so appended children fill their runs. Runs are storage, like tree-sitter's hidden
 * repetition nodes: the children are their tier-zero entries in order, and nothing else sees a run.
 *
 * A run is a shared value with a reference count, as a node is. A tree that
 * holds a run another tree holds copies it before changing it, so a change
 * copies only the runs on the path to what changed. A node with no children
 * holds no run.
 *
 * Every operation that can allocate makes its copies and new runs before it
 * changes anything, so a failure leaves the tree as it was. */
#define MARKDOWN_CORE_RUN_WIDTH 16
/* Tiers a tree can have: a tree of this many tiers, its runs half full,
 * holds more children than an int32 offset can address. */
#define MARKDOWN_CORE_RUN_TIERS 12

typedef struct markdown_core_run {
    /* The run's holders, or, once it has none, the next run of the release
     * list it is on (node.c). */
    union {
        size_t refs;
        struct markdown_core_run *released;
    } hold;
    /* The children under the run. */
    uint32_t total;
    uint8_t count;
    uint8_t tier;
    void *entries[MARKDOWN_CORE_RUN_WIDTH];
} markdown_core_run;

static inline size_t markdown_core_children_count(const markdown_core_run *run) { return run ? run->total : 0; }

/* The entry of `run`, a run above tier zero, that holds the position
 * `*index`, which becomes the position within that entry. A position below
 * the count is a child's; an insertion's may be the count, and a position
 * between two entries is in either. The search starts from the nearer end
 * of the run, so an end of the tree is found in one step per tier. */
static inline size_t markdown_core_run_find(const markdown_core_run *run, size_t *index) {
    size_t total, k;
    if (*index < run->total / 2) {
        k = 0;
        while ((total = ((const markdown_core_run *)run->entries[k])->total) <= *index) {
            *index -= total;
            k++;
        }
        return k;
    }
    size_t after = run->total - *index;
    k = run->count - 1u;
    while ((total = ((const markdown_core_run *)run->entries[k])->total) < after) {
        after -= total;
        k--;
    }
    *index = total - after;
    return k;
}

/* The tier-zero run that holds the child at `*index`, which is below the
 * count and becomes the child's place in that run. */
static inline const markdown_core_run *markdown_core_children_leaf(const markdown_core_run *run, size_t *index) {
    while (run->tier) {
        run = (const markdown_core_run *)run->entries[markdown_core_run_find(run, index)];
    }
    return run;
}

/* The child at `index`, which is below the count. */
static inline struct markdown_core_node *markdown_core_children_at(const markdown_core_run *run, size_t index) {
    run = markdown_core_children_leaf(run, &index);
    return (struct markdown_core_node *)run->entries[index];
}

/* The last child; the tree has one. */
static inline struct markdown_core_node *markdown_core_children_last(const markdown_core_run *run) {
    while (run->tier) {
        run = (const markdown_core_run *)run->entries[run->count - 1];
    }
    return (struct markdown_core_node *)run->entries[run->count - 1];
}

/* An empty run of `tier`, held once, from `pool` (NULL: the allocator); NULL
 * when storage runs out. */
static inline markdown_core_run *markdown_core_run_new(markdown_core_node_pool *pool, uint8_t tier) {
    markdown_core_run *run = (markdown_core_run *)markdown_core_slab_take(pool ? &pool->slabs : NULL,
                                                                          pool ? &pool->runs : NULL, sizeof(*run));
    if (run) {
        run->hold.refs = 1;
        run->total = 0;
        run->count = 0;
        run->tier = tier;
    }
    return run;
}

/* Puts `node` at `index`, at most the count, taking the caller's reference.
 * False, with nothing changed and the reference still the caller's, when
 * storage runs out. */
bool markdown_core_children_insert(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                   struct markdown_core_node *node);

/* `markdown_core_children_insert` at the count. Along the path of last
 * entries, a tree held only here with room in its last tier-zero run takes
 * the child in place; any other copies and splits as the insertion does. */
static inline bool markdown_core_children_append(markdown_core_node_pool *pool, markdown_core_run **root,
                                                 struct markdown_core_node *node) {
    markdown_core_run *run = *root;
    if (!run) {
        /* The first child makes the tree: one tier-zero run holding it. */
        if (!(run = markdown_core_run_new(pool, 0))) {
            return false;
        }
        run->entries[0] = node;
        run->count = 1;
        run->total = 1;
        *root = run;
        return true;
    }
    while (run->hold.refs == 1 && run->tier) {
        run = (markdown_core_run *)run->entries[run->count - 1];
    }
    if (run->hold.refs != 1 || run->count == MARKDOWN_CORE_RUN_WIDTH) {
        return markdown_core_children_insert(pool, root, (*root)->total, node);
    }
    run->entries[run->count++] = node;
    for (run = *root; run->tier; run = (markdown_core_run *)run->entries[run->count - 1]) {
        run->total++;
    }
    run->total++;
    return true;
}

/* `remove`'s step when a run on its path is shared or its tier-zero run
 * would fall below what it must hold: the runs on the path are copied, and
 * each borrows from or joins a neighbour as it must. */
bool markdown_core_children_remove_joining(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                           struct markdown_core_node **removed);

/* Takes the child at `index` out, handing its reference to the caller in
 * `removed`. False, with nothing changed, when storage runs out. A tree held
 * only here whose tier-zero run keeps another entry, and half the width when
 * it is not the last of its tier, gives the child up in place. */
static inline bool markdown_core_children_remove(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                                 struct markdown_core_node **removed) {
    markdown_core_run *run = *root;
    size_t at = index;
    bool last = true;
    while (run->hold.refs == 1 && run->tier) {
        size_t k = markdown_core_run_find(run, &at);
        last = last && k + 1 == run->count;
        run = (markdown_core_run *)run->entries[k];
    }
    if (run->hold.refs != 1 || run->count == 1 || (!last && run->count <= MARKDOWN_CORE_RUN_WIDTH / 2)) {
        return markdown_core_children_remove_joining(pool, root, index, removed);
    }
    for (run = *root; run->tier;) {
        markdown_core_run *entry = (markdown_core_run *)run->entries[markdown_core_run_find(run, &index)];
        run->total--;
        run = entry;
    }
    *removed = (struct markdown_core_node *)run->entries[index];
    run->count--;
    run->total--;
    for (size_t i = index; i < run->count; i++) {
        run->entries[i] = run->entries[i + 1];
    }
    return true;
}

/* Puts `node` at `index` in place of the child there, taking the caller's
 * reference and handing the old child's to the caller in `replaced`. False,
 * with nothing changed, when storage runs out. */
bool markdown_core_children_replace(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                    struct markdown_core_node *node, struct markdown_core_node **replaced);

/* A TREE BUILT FROM A SEQUENCE: an open run of inline nodes freezing into
 * its owner's children (5.11). `put` fills tier-zero runs in order, each to
 * the width, and `end` joins the tier's runs under the tier above the same
 * way, until one run, the root, holds them all: the tree appending them one
 * by one would make. Nothing the caller
 * hands over is the builder's until `end` succeeds: on a failure the builder
 * gives back its runs and the caller still holds every child it put. */
typedef struct {
    markdown_core_node_pool *pool;
    /* The tier-zero runs filled so far, in order, linked through their
     * release link. */
    markdown_core_run *first, *last;
} markdown_core_children_builder;

/* Gives back the filled runs from `first` on, leaving their entries the
 * caller's. The builder's out-of-line steps take its fields, not the
 * builder, so that the builder stays in registers. */
void markdown_core_children_build_cancel(markdown_core_node_pool *pool, markdown_core_run *first);
/* `end` when more than one run is filled: the root, or NULL, with the runs
 * given back, when storage runs out. */
markdown_core_run *markdown_core_children_build_join(markdown_core_node_pool *pool, markdown_core_run *first);

static inline void markdown_core_children_build_begin(markdown_core_children_builder *builder,
                                                      markdown_core_node_pool *pool) {
    builder->pool = pool;
    builder->first = NULL;
    builder->last = NULL;
}

/* Puts the next child. False, with the builder cancelled, when storage runs
 * out. */
static inline bool markdown_core_children_build_put(markdown_core_children_builder *builder,
                                                    struct markdown_core_node *node) {
    markdown_core_run *run = builder->last;
    if (!run || run->count == MARKDOWN_CORE_RUN_WIDTH) {
        run = markdown_core_run_new(builder->pool, 0);
        if (!run) {
            markdown_core_children_build_cancel(builder->pool, builder->first);
            return false;
        }
        run->hold.released = NULL;
        if (builder->last) {
            builder->last->hold.released = run;
        } else {
            builder->first = run;
        }
        builder->last = run;
    }
    run->entries[run->count++] = node;
    run->total++;
    return true;
}

/* The tree of the children put, held once, and NULL for none. False in `ok`,
 * with the builder cancelled, when storage runs out. */
static inline markdown_core_run *markdown_core_children_build_end(markdown_core_children_builder *builder, bool *ok) {
    if (builder->first != builder->last) {
        markdown_core_run *root = markdown_core_children_build_join(builder->pool, builder->first);
        *ok = root != NULL;
        return root;
    }
    *ok = true;
    if (builder->first) {
        builder->first->hold.refs = 1;
    }
    return builder->first;
}

/* The number of broken invariants in the tree: a run with no holder, an
 * empty or overfull run, a run less than half full that is not the last of
 * its tier, a total
 * that is not its entries' sum, or an entry of the wrong tier. */
size_t markdown_core_children_check(const markdown_core_run *root);

/* A run another holder now holds too. */
static inline markdown_core_run *markdown_core_run_retain(markdown_core_run *run) {
    if (run) {
        run->hold.refs++;
    }
    return run;
}

/* A WALK OVER CHILDREN IN ORDER, from a position: the path from the root to
 * the tier-zero run of the next child. */
typedef struct {
    const markdown_core_run *runs[MARKDOWN_CORE_RUN_TIERS];
    uint8_t at[MARKDOWN_CORE_RUN_TIERS];
    /* Tiers on the path; zero once the walk is past the last child. */
    int tiers;
} markdown_core_children_cursor;

/* Places `cursor` at the child at `index`, which may be the count. */
void markdown_core_children_seek(markdown_core_children_cursor *cursor, const markdown_core_run *root, size_t index);

/* The child at the cursor, moving past it, or NULL past the last. */
static inline struct markdown_core_node *markdown_core_children_next(markdown_core_children_cursor *cursor) {
    int top = cursor->tiers - 1;
    if (top < 0) {
        return NULL;
    }
    const markdown_core_run *run = cursor->runs[top];
    struct markdown_core_node *node = (struct markdown_core_node *)run->entries[cursor->at[top]];
    if (++cursor->at[top] < run->count) {
        return node;
    }
    /* Up to the first run with an entry after the one walked, then down
     * its first entries to tier zero. */
    while (top >= 0 && cursor->at[top] >= cursor->runs[top]->count) {
        top--;
        if (top >= 0) {
            cursor->at[top]++;
        }
    }
    if (top < 0) {
        cursor->tiers = 0;
        return node;
    }
    while (top + 1 < cursor->tiers) {
        const markdown_core_run *down = (const markdown_core_run *)cursor->runs[top]->entries[cursor->at[top]];
        top++;
        cursor->runs[top] = down;
        cursor->at[top] = 0;
    }
    return node;
}

#ifdef __cplusplus
}
#endif

#endif
