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
struct markdown_core_node_pool;

/* A NODE'S CHILDREN, as a balanced tree of shared runs
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.1 and 5.11).
 *
 * A run holds up to MARKDOWN_CORE_RUN_WIDTH entries: nodes when its tier is
 * zero, and runs of the tier below otherwise. Every run of a tree that is not
 * its root holds at least half the width, and every tier-zero run is at the
 * same depth, so finding, inserting and removing a child costs O(log
 * children). Runs are storage, like tree-sitter's hidden repetition nodes:
 * the children are their tier-zero entries in order, and nothing else sees a
 * run.
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

/* Puts `node` at `index`, at most the count, taking the caller's reference.
 * False, with nothing changed and the reference still the caller's, when
 * storage runs out. */
bool markdown_core_children_insert(struct markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                   struct markdown_core_node *node);

static inline bool markdown_core_children_append(struct markdown_core_node_pool *pool, markdown_core_run **root,
                                                 struct markdown_core_node *node) {
    return markdown_core_children_insert(pool, root, markdown_core_children_count(*root), node);
}

/* Takes the child at `index` out, handing its reference to the caller in
 * `removed`. False, with nothing changed, when storage runs out. */
bool markdown_core_children_remove(struct markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                   struct markdown_core_node **removed);

/* Puts `node` at `index` in place of the child there, taking the caller's
 * reference and handing the old child's to the caller in `replaced`. False,
 * with nothing changed, when storage runs out. */
bool markdown_core_children_replace(struct markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                    struct markdown_core_node *node, struct markdown_core_node **replaced);

/* Runs come from slabs of this size (slab.h). */
#define MARKDOWN_CORE_RUN_SLAB_BYTES ((size_t)32 * 1024)

/* An empty run of `tier`, held once, from `runs` (NULL: the allocator); NULL
 * when storage runs out. */
static inline markdown_core_run *markdown_core_run_new(markdown_core_slab_pool *runs, uint8_t tier) {
    markdown_core_run *run =
        (markdown_core_run *)markdown_core_slab_take(runs, sizeof(markdown_core_run), MARKDOWN_CORE_RUN_SLAB_BYTES);
    if (run) {
        run->hold.refs = 1;
        run->total = 0;
        run->count = 0;
        run->tier = tier;
    }
    return run;
}

/* A TREE BUILT FROM A SEQUENCE, by a builder that knows how many children it
 * is handed: an open run of inline nodes freezing into its owner's children
 * (5.11). `begin` makes every run the tree needs, `put` fills each tier's
 * runs in order and cannot fail, and `end` answers the root once every child
 * is put. The runs of a tier share its entries evenly, so each run below the
 * root holds at least half the width. */
typedef struct {
    /* The run the tier is filling, NULL between runs. */
    markdown_core_run *open;
    /* Entries the open run still takes; each run's share; and the runs, of
     * those not yet begun, that take one more. */
    uint32_t left, share, extra;
} markdown_core_children_tier;

typedef struct {
    markdown_core_slab_pool *runs;
    /* Runs made and not yet begun, linked through their release link. */
    markdown_core_run *spare;
    markdown_core_run *root;
    int tiers;
    markdown_core_children_tier tier[MARKDOWN_CORE_RUN_TIERS];
} markdown_core_children_builder;

/* Gives back the runs a builder made and has not begun. */
void markdown_core_children_build_cancel(markdown_core_children_builder *builder);
/* `put`'s step when a run below the top tier is full: carries it up. */
void markdown_core_children_build_carry(markdown_core_children_builder *builder, markdown_core_run *run);

/* Readies `builder` for `count` children, taking runs from `runs`. False,
 * with nothing made, when storage runs out. */
static inline bool markdown_core_children_build_begin(markdown_core_children_builder *builder,
                                                      markdown_core_slab_pool *runs, size_t count) {
    builder->runs = runs;
    builder->spare = NULL;
    builder->root = NULL;
    builder->tiers = 0;
    size_t made = 0;
    while (count) {
        size_t share = (count + MARKDOWN_CORE_RUN_WIDTH - 1) / MARKDOWN_CORE_RUN_WIDTH;
        markdown_core_children_tier *tier = &builder->tier[builder->tiers++];
        tier->open = NULL;
        tier->share = (uint32_t)(count / share);
        tier->extra = (uint32_t)(count % share);
        made += share;
        count = share > 1 ? share : 0;
    }
    while (made--) {
        markdown_core_run *run = markdown_core_run_new(runs, 0);
        if (!run) {
            markdown_core_children_build_cancel(builder);
            return false;
        }
        run->hold.released = builder->spare;
        builder->spare = run;
    }
    return true;
}

/* The tier's next run, begun. */
static inline markdown_core_run *markdown_core_children_build_open(markdown_core_children_builder *builder,
                                                                   markdown_core_children_tier *tier) {
    markdown_core_run *run = builder->spare;
    builder->spare = run->hold.released;
    run->hold.refs = 1;
    tier->open = run;
    tier->left = tier->share + (tier->extra != 0);
    tier->extra -= tier->extra != 0;
    return run;
}

/* Puts the next child, taking the caller's reference. */
static inline void markdown_core_children_build_put(markdown_core_children_builder *builder,
                                                    struct markdown_core_node *node) {
    markdown_core_children_tier *tier = &builder->tier[0];
    markdown_core_run *run = tier->open ? tier->open : markdown_core_children_build_open(builder, tier);
    run->entries[run->count++] = node;
    run->total++;
    if (--tier->left) {
        return;
    }
    tier->open = NULL;
    if (builder->tiers == 1) {
        builder->root = run;
    } else {
        markdown_core_children_build_carry(builder, run);
    }
}

/* The tree of the children put, NULL for none. */
static inline markdown_core_run *markdown_core_children_build_end(markdown_core_children_builder *builder) {
    return builder->root;
}

/* The number of broken invariants in the tree: a run with no holder, an
 * empty or overfull run, a run below the root less than half full, a total
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
