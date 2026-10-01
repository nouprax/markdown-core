#include <string.h>

#include "children.h"
#include "node.h"
#include "slab.h"

#define S_HALF (MARKDOWN_CORE_RUN_WIDTH / 2)

/* A run's storage, its entries having moved elsewhere or been released. */
void markdown_core_run_free_slot(markdown_core_node_pool *pool, markdown_core_run *run) {
    markdown_core_slab_release(pool ? &pool->runs : NULL, run);
}

/* A copy of the shared run in `slot`, put in its place: the copy holds
 * every entry once more, and the run loses this tree as a holder. */
static bool S_copy(markdown_core_node_pool *pool, markdown_core_run **slot) {
    markdown_core_run *run = *slot;
    markdown_core_run *copy = markdown_core_run_new(pool, run->tier);
    if (!copy) {
        return false;
    }
    copy->count = run->count;
    copy->total = run->total;
    memcpy(copy->entries, run->entries, run->count * sizeof(run->entries[0]));
    for (size_t i = 0; i < run->count; i++) {
        if (run->tier) {
            markdown_core_run_retain((markdown_core_run *)run->entries[i]);
        } else {
            markdown_core_node_retain((markdown_core_node *)run->entries[i]);
        }
    }
    /* Shared, so this holder was not its last. */
    run->hold.refs--;
    *slot = copy;
    return true;
}

/* The run in `slot`, which this tree alone then holds. */
static inline bool S_unshare(markdown_core_node_pool *pool, markdown_core_run **slot) {
    return (*slot)->hold.refs == 1 || S_copy(pool, slot);
}

static void S_recount(markdown_core_run *run) {
    if (!run->tier) {
        run->total = run->count;
        return;
    }
    size_t total = 0;
    for (size_t i = 0; i < run->count; i++) {
        total += ((const markdown_core_run *)run->entries[i])->total;
    }
    run->total = (uint32_t)total;
}

/* Entries move one at a time: a run is narrow, and an insertion at its end,
 * the usual place, moves none. */
static inline void S_put(markdown_core_run *run, size_t at, void *entry) {
    for (size_t i = run->count; i > at; i--) {
        run->entries[i] = run->entries[i - 1];
    }
    run->entries[at] = entry;
    run->count++;
}

static inline void *S_take(markdown_core_run *run, size_t at) {
    void *entry = run->entries[at];
    run->count--;
    for (size_t i = at; i < run->count; i++) {
        run->entries[i] = run->entries[i + 1];
    }
    return entry;
}

/* THE PATH TO A POSITION: each run from the root down, all held by this
 * tree alone, and the entry taken in each (markdown_core_run_find). */
typedef struct {
    markdown_core_run *runs[MARKDOWN_CORE_RUN_TIERS];
    size_t at[MARKDOWN_CORE_RUN_TIERS];
    int tiers;
} S_path;

static bool S_descend(markdown_core_node_pool *pool, markdown_core_run **root, size_t index, S_path *path) {
    markdown_core_run **slot = root;
    path->tiers = 0;
    for (;;) {
        if (!S_unshare(pool, slot)) {
            return false;
        }
        markdown_core_run *run = *slot;
        int tier = path->tiers++;
        path->runs[tier] = run;
        if (!run->tier) {
            path->at[tier] = index;
            return true;
        }
        size_t k = markdown_core_run_find(run, &index);
        path->at[tier] = k;
        slot = (markdown_core_run **)&run->entries[k];
    }
}

/* The insertion of `node` at `at` in the full tier-zero run of `path`: each
 * full run from tier zero up splits in halves, and a root goes above them
 * when every run on the path is full. The new runs are made before anything
 * changes. */
static bool S_insert_splitting(markdown_core_node_pool *pool, markdown_core_run **root, S_path *path, size_t at,
                               markdown_core_node *node) {
    markdown_core_run *made[MARKDOWN_CORE_RUN_TIERS + 1];
    int splits = 0;
    while (splits < path->tiers && path->runs[path->tiers - 1 - splits]->count == MARKDOWN_CORE_RUN_WIDTH) {
        splits++;
    }
    int needed = splits + (splits == path->tiers);
    for (int i = 0; i < needed; i++) {
        made[i] = markdown_core_run_new(pool, 0);
        if (!made[i]) {
            while (i--) {
                markdown_core_run_free_slot(pool, made[i]);
            }
            return false;
        }
    }
    void *entry = node;
    int used = 0;
    for (int tier = path->tiers - 1; tier >= 0; tier--) {
        markdown_core_run *run = path->runs[tier];
        if (!entry) {
            run->total++;
            continue;
        }
        if (run->count < MARKDOWN_CORE_RUN_WIDTH) {
            S_put(run, at, entry);
            run->total++;
            entry = NULL;
        } else {
            /* Split the full run in halves and put the entry in its half; the
             * right half then goes into the parent after the left. */
            markdown_core_run *right = made[used++];
            right->tier = run->tier;
            memcpy(right->entries, &run->entries[S_HALF], S_HALF * sizeof(run->entries[0]));
            right->count = S_HALF;
            run->count = S_HALF;
            if (at <= S_HALF) {
                S_put(run, at, entry);
            } else {
                S_put(right, at - S_HALF, entry);
            }
            S_recount(run);
            S_recount(right);
            entry = right;
        }
        if (tier) {
            at = path->at[tier - 1] + 1;
        }
    }
    if (entry) {
        markdown_core_run *top = made[used];
        top->tier = (uint8_t)(path->runs[0]->tier + 1);
        top->entries[0] = path->runs[0];
        top->entries[1] = entry;
        top->count = 2;
        S_recount(top);
        *root = top;
    }
    return true;
}

bool markdown_core_children_insert(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                   markdown_core_node *node) {
    if (!*root) {
        markdown_core_run *run = markdown_core_run_new(pool, 0);
        if (!run) {
            return false;
        }
        run->entries[0] = node;
        run->count = 1;
        run->total = 1;
        *root = run;
        return true;
    }
    S_path path;
    if (!S_descend(pool, root, index, &path)) {
        return false;
    }
    int leaf = path.tiers - 1;
    markdown_core_run *run = path.runs[leaf];
    if (run->count == MARKDOWN_CORE_RUN_WIDTH) {
        return S_insert_splitting(pool, root, &path, path.at[leaf], node);
    }
    S_put(run, path.at[leaf], node);
    for (int tier = 0; tier <= leaf; tier++) {
        path.runs[tier]->total++;
    }
    return true;
}

bool markdown_core_children_replace(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                    markdown_core_node *node, markdown_core_node **replaced) {
    S_path path;
    if (!S_descend(pool, root, index, &path)) {
        return false;
    }
    markdown_core_run *run = path.runs[path.tiers - 1];
    *replaced = (markdown_core_node *)run->entries[path.at[path.tiers - 1]];
    run->entries[path.at[path.tiers - 1]] = node;
    return true;
}

bool markdown_core_children_remove_joining(markdown_core_node_pool *pool, markdown_core_run **root, size_t index,
                                           markdown_core_node **removed) {
    S_path path;
    if (!S_descend(pool, root, index, &path)) {
        return false;
    }
    /* The neighbour each run on the path may borrow from or join, held by
     * this tree alone before anything changes. */
    for (int tier = 1; tier < path.tiers; tier++) {
        markdown_core_run *parent = path.runs[tier - 1];
        size_t k = path.at[tier - 1];
        size_t neighbour = k ? k - 1 : k + 1;
        if (neighbour < parent->count && !S_unshare(pool, (markdown_core_run **)&parent->entries[neighbour])) {
            return false;
        }
    }
    markdown_core_run *leaf = path.runs[path.tiers - 1];
    *removed = (markdown_core_node *)S_take(leaf, path.at[path.tiers - 1]);
    for (int tier = 0; tier < path.tiers; tier++) {
        path.runs[tier]->total--;
    }
    /* Restore every run's half from tier zero up: borrow an entry from a
     * neighbour that can spare one, or else join the run and its neighbour,
     * which takes an entry from the parent. */
    for (int tier = path.tiers - 1; tier > 0; tier--) {
        markdown_core_run *run = path.runs[tier];
        if (run->count >= S_HALF) {
            break;
        }
        markdown_core_run *parent = path.runs[tier - 1];
        size_t k = path.at[tier - 1];
        size_t neighbour = k ? k - 1 : k + 1;
        markdown_core_run *other = (markdown_core_run *)parent->entries[neighbour];
        if (other->count > S_HALF) {
            if (neighbour < k) {
                void *entry = S_take(other, other->count - 1);
                S_put(run, 0, entry);
            } else {
                S_put(run, run->count, S_take(other, 0));
            }
            S_recount(run);
            S_recount(other);
            break;
        }
        markdown_core_run *left = neighbour < k ? other : run, *right = neighbour < k ? run : other;
        memcpy(&left->entries[left->count], right->entries, right->count * sizeof(right->entries[0]));
        left->count = (uint8_t)(left->count + right->count);
        left->total += right->total;
        S_take(parent, neighbour < k ? k : neighbour);
        markdown_core_run_free_slot(pool, right);
    }
    markdown_core_run *top = *root;
    if (!top->count) {
        markdown_core_run_free_slot(pool, top);
        *root = NULL;
        return true;
    }
    while (top->tier && top->count == 1) {
        markdown_core_run *only = (markdown_core_run *)top->entries[0];
        markdown_core_run_free_slot(pool, top);
        top = only;
    }
    *root = top;
    return true;
}

void markdown_core_children_build_cancel(markdown_core_node_pool *pool, markdown_core_run *first) {
    while (first) {
        markdown_core_run *run = first;
        first = run->hold.released;
        markdown_core_run_free_slot(pool, run);
    }
}

markdown_core_run *markdown_core_children_build_join(markdown_core_node_pool *pool, markdown_core_run *first) {
    /* Every run of the tiers above, made before any tier is joined. */
    size_t below = 0;
    for (markdown_core_run *run = first; run; run = run->hold.released) {
        below++;
    }
    markdown_core_run *spare = NULL;
    for (size_t made = below; made > 1;) {
        made = (made + MARKDOWN_CORE_RUN_WIDTH - 1) / MARKDOWN_CORE_RUN_WIDTH;
        for (size_t i = 0; i < made; i++) {
            markdown_core_run *run = markdown_core_run_new(pool, 0);
            if (!run) {
                markdown_core_children_build_cancel(pool, spare);
                markdown_core_children_build_cancel(pool, first);
                return NULL;
            }
            run->hold.released = spare;
            spare = run;
        }
    }
    for (;;) {
        /* Every run of the tier is full but the last; it takes half of what
         * it and the one before it hold when it has less. */
        markdown_core_run *before = first, *last;
        while ((last = before->hold.released)->hold.released) {
            before = last;
        }
        if (last->count < S_HALF) {
            size_t moved = (size_t)(before->count + last->count) / 2 - last->count;
            memmove(&last->entries[moved], last->entries, last->count * sizeof(last->entries[0]));
            memcpy(last->entries, &before->entries[before->count - moved], moved * sizeof(last->entries[0]));
            uint32_t total = (uint32_t)moved;
            if (last->tier) {
                total = 0;
                for (size_t i = 0; i < moved; i++) {
                    total += ((markdown_core_run *)last->entries[i])->total;
                }
            }
            before->count = (uint8_t)(before->count - moved);
            before->total -= total;
            last->count = (uint8_t)(last->count + moved);
            last->total += total;
        }
        /* The tier's runs, in order, under the runs of the tier above. */
        markdown_core_run *above_first = NULL, *above = NULL;
        while (first) {
            markdown_core_run *next = first->hold.released;
            if (!above || above->count == MARKDOWN_CORE_RUN_WIDTH) {
                markdown_core_run *run = spare;
                spare = run->hold.released;
                run->tier = (uint8_t)(first->tier + 1);
                run->hold.released = NULL;
                if (above) {
                    above->hold.released = run;
                } else {
                    above_first = run;
                }
                above = run;
            }
            first->hold.refs = 1;
            above->entries[above->count++] = first;
            above->total += first->total;
            first = next;
        }
        first = above_first;
        if (!first->hold.released) {
            break;
        }
    }
    first->hold.refs = 1;
    return first;
}

void markdown_core_children_seek(markdown_core_children_cursor *cursor, const markdown_core_run *root, size_t index) {
    cursor->tiers = 0;
    if (!root || index >= root->total) {
        return;
    }
    const markdown_core_run *run = root;
    for (;;) {
        int tier = cursor->tiers++;
        cursor->runs[tier] = run;
        if (!run->tier) {
            cursor->at[tier] = (uint8_t)index;
            return;
        }
        size_t k = markdown_core_run_find(run, &index);
        cursor->at[tier] = (uint8_t)k;
        run = (const markdown_core_run *)run->entries[k];
    }
}

size_t markdown_core_children_check(const markdown_core_run *root) {
    if (!root) {
        return 0;
    }
    /* Depth first over the runs, at most a run's width pending per tier. */
    const markdown_core_run *pending[MARKDOWN_CORE_RUN_TIERS * MARKDOWN_CORE_RUN_WIDTH];
    size_t count = 0, errors = 0;
    if (root->tier >= MARKDOWN_CORE_RUN_TIERS) {
        return 1;
    }
    pending[count++] = root;
    while (count) {
        const markdown_core_run *run = pending[--count];
        errors += !run->hold.refs || !run->count || run->count > MARKDOWN_CORE_RUN_WIDTH ||
                  (run != root && run->count < S_HALF);
        if (!run->tier) {
            errors += run->total != run->count;
            for (size_t i = 0; i < run->count; i++) {
                const markdown_core_node *node = run->entries[i];
                errors += !node || !node->hold.refs;
            }
            continue;
        }
        size_t total = 0;
        for (size_t i = 0; i < run->count; i++) {
            const markdown_core_run *entry = run->entries[i];
            if (!entry || entry->tier + 1 != run->tier) {
                errors++;
                continue;
            }
            total += entry->total;
            pending[count++] = entry;
        }
        errors += total != run->total;
    }
    return errors;
}
