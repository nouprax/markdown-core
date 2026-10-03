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
    copy->sealed = run->sealed;
    copy->marks = run->marks;
    copy->reach = run->reach;
    copy->tally = run->tally;
    copy->length = run->length;
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
    run->sealed = 0;
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
    run->sealed = 0;
}

static inline void *S_take(markdown_core_run *run, size_t at) {
    void *entry = run->entries[at];
    run->count--;
    run->sealed = 0;
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
        /* Every caller changes the runs on its path. */
        run->sealed = 0;
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
 * full run from tier zero up splits, and a root goes above them when every
 * run on the path is full. A run splits in halves, except at the tree's end,
 * where it stays full and the entry starts the next run. The new runs are
 * made before anything changes. */
static bool S_insert_splitting(markdown_core_node_pool *pool, markdown_core_run **root, S_path *path, size_t at,
                               markdown_core_node *node) {
    bool end = at == path->runs[path->tiers - 1]->count;
    for (int tier = 0; end && tier < path->tiers - 1; tier++) {
        end = path->at[tier] + 1 == path->runs[tier]->count;
    }
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
        } else if (end) {
            /* The entry starts the run after the full last run of its tier,
             * which goes into the parent after it. */
            markdown_core_run *next = made[used++];
            next->tier = run->tier;
            next->entries[0] = entry;
            next->count = 1;
            S_recount(next);
            entry = next;
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
    /* The runs on the path that are the last of their tier. */
    bool last[MARKDOWN_CORE_RUN_TIERS];
    last[0] = true;
    for (int tier = 1; tier < path.tiers; tier++) {
        last[tier] = last[tier - 1] && path.at[tier - 1] + 1 == path.runs[tier - 1]->count;
    }
    markdown_core_run *leaf = path.runs[path.tiers - 1];
    *removed = (markdown_core_node *)S_take(leaf, path.at[path.tiers - 1]);
    for (int tier = 0; tier < path.tiers; tier++) {
        path.runs[tier]->total--;
    }
    /* Restore what every run must hold from tier zero up: borrow an entry
     * from a neighbour that can spare one, or else join the run and its
     * neighbour, which takes an entry from the parent. A last run left empty
     * that is its parent's only entry leaves the parent empty in its turn. */
    for (int tier = path.tiers - 1; tier > 0; tier--) {
        markdown_core_run *run = path.runs[tier];
        if (run->count >= (last[tier] ? 1u : S_HALF)) {
            break;
        }
        markdown_core_run *parent = path.runs[tier - 1];
        size_t k = path.at[tier - 1];
        if (parent->count == 1) {
            S_take(parent, 0);
            markdown_core_run_free_slot(pool, run);
            continue;
        }
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
        left->sealed = 0;
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
    /* Whether each pending run is the last of its tier. */
    bool last[MARKDOWN_CORE_RUN_TIERS * MARKDOWN_CORE_RUN_WIDTH];
    size_t count = 0, errors = 0;
    if (root->tier >= MARKDOWN_CORE_RUN_TIERS) {
        return 1;
    }
    pending[count] = root;
    last[count++] = true;
    while (count) {
        const markdown_core_run *run = pending[--count];
        bool ends = last[count];
        errors +=
            !run->hold.refs || !run->count || run->count > MARKDOWN_CORE_RUN_WIDTH || (!ends && run->count < S_HALF);
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
            pending[count] = entry;
            last[count++] = ends && i + 1 == run->count;
        }
        errors += total != run->total;
    }
    return errors;
}

/* RANGES OF CHILDREN (docs/plans/2026-09-29-incremental-parsing.md, 5.3): a
 * parse that takes a run of an old node's children puts that range of the
 * old tree into the new one, sharing every run that lies wholly inside it,
 * so taking k children costs O(log k), not k.
 *
 * A slice is a tree whose runs hold at least half the width except along
 * its two spines, the paths of first and of last entries: those are the
 * copies its cuts made. A join first restores half the width along the
 * spines that the join makes interior -- the left tree's last entries and
 * the right tree's first ones -- and then hangs the shorter tree on the
 * other's spine at its own tier, as an insertion does. Both touch O(log n)
 * runs, and copy those that are shared. */

/* Holds the entry at `at` of `run` once more. */
static inline void S_retain_entry(const markdown_core_run *run, size_t at) {
    if (run->tier) {
        markdown_core_run_retain((markdown_core_run *)run->entries[at]);
    } else {
        markdown_core_node_retain((markdown_core_node *)run->entries[at]);
    }
}

/* The entry of `run` a cut at `*index` divides, `*index` becoming the cut's
 * place in it: for a suffix, the entry that holds the child at the cut, and
 * for a prefix, the one that holds the child before it, so the cut is never
 * at the start of the entry it divides. */
static inline size_t S_cut_entry(const markdown_core_run *run, size_t *index, bool suffix) {
    if (suffix) {
        return markdown_core_run_find(run, index);
    }
    --*index;
    size_t k = markdown_core_run_find(run, index);
    ++*index;
    return k;
}

/* Whether a cut at `index` divides `run`: a suffix from the first child or a
 * prefix to the last takes it whole. */
static inline bool S_divides(const markdown_core_run *run, size_t index, bool suffix) {
    return suffix ? index != 0 : index != run->total;
}

/* The runs a cut through `run` at `index` copies, from `run` down: each run
 * whose entries the cut divides. */
static int S_cut_depth(const markdown_core_run *run, size_t index, bool suffix) {
    int depth = 0;
    while (S_divides(run, index, suffix)) {
        depth++;
        if (!run->tier) {
            break;
        }
        run = run->entries[S_cut_entry(run, &index, suffix)];
    }
    return depth;
}

/* `run`'s children from the one at `index` on (`suffix`), or before it, as a
 * tree whose spine on that side is copies taken from `made`, which holds
 * enough of them: a run the cut does not divide is shared whole. */
static void *S_cut(markdown_core_run **made, int *used, markdown_core_run *run, size_t index, bool suffix) {
    void *top = NULL, **slot = &top;
    for (;;) {
        if (!S_divides(run, index, suffix)) {
            markdown_core_run_retain(run);
            *slot = run;
            break;
        }
        markdown_core_run *copy = made[(*used)++];
        copy->tier = run->tier;
        *slot = copy;
        if (!run->tier) {
            size_t first = suffix ? index : 0, last = suffix ? run->count : index;
            for (size_t i = first; i < last; i++) {
                copy->entries[copy->count++] = run->entries[i];
                S_retain_entry(run, i);
            }
            break;
        }
        size_t k = S_cut_entry(run, &index, suffix);
        size_t first = suffix ? k + 1 : 0, last = suffix ? run->count : k;
        /* The divided entry's place: first for a suffix, last for a prefix. */
        copy->count = suffix;
        for (size_t i = first; i < last; i++) {
            copy->entries[copy->count++] = run->entries[i];
            S_retain_entry(run, i);
        }
        copy->count = (uint8_t)(copy->count + !suffix);
        slot = &copy->entries[suffix ? 0 : copy->count - 1u];
        run = run->entries[k];
    }
    return top;
}

/* The totals of the copies `made[from, to)`, deepest first. */
static void S_recount_made(markdown_core_run **made, int from, int to) {
    while (to-- > from) {
        S_recount(made[to]);
    }
}

markdown_core_run *markdown_core_children_slice(markdown_core_node_pool *pool, markdown_core_run *root, size_t first,
                                                size_t count, bool *ok) {
    *ok = true;
    if (!count) {
        return NULL;
    }
    size_t last = first + count;
    markdown_core_run *run = root;
    /* Down to the run in which the range divides two entries, or to the
     * tier-zero run that holds it. */
    while (run->tier && (first || last != run->total)) {
        size_t a = first, b = last - 1;
        size_t k = markdown_core_run_find(run, &a), j = markdown_core_run_find(run, &b);
        if (k != j) {
            break;
        }
        first = a;
        last = b + 1;
        run = run->entries[k];
    }
    if (!first && last == run->total) {
        return markdown_core_run_retain(run);
    }
    /* The top copy, and the copies of the cuts below its first and last
     * entries, made before anything is put in them. */
    markdown_core_run *made[2 * MARKDOWN_CORE_RUN_TIERS + 1];
    size_t a = first, b = last - 1;
    size_t k = run->tier ? markdown_core_run_find(run, &a) : 0, j = run->tier ? markdown_core_run_find(run, &b) : 0;
    int needed = 1;
    if (run->tier) {
        needed += S_cut_depth(run->entries[k], a, true) + S_cut_depth(run->entries[j], b + 1, false);
    }
    for (int i = 0; i < needed; i++) {
        if (!(made[i] = markdown_core_run_new(pool, 0))) {
            while (i--) {
                markdown_core_run_free_slot(pool, made[i]);
            }
            *ok = false;
            return NULL;
        }
    }
    markdown_core_run *top = made[0];
    top->tier = run->tier;
    int used = 1;
    if (!run->tier) {
        for (size_t i = first; i < last; i++) {
            top->entries[top->count++] = run->entries[i];
            S_retain_entry(run, i);
        }
        S_recount(top);
        return top;
    }
    top->entries[top->count++] = S_cut(made, &used, run->entries[k], a, true);
    int prefix = used;
    for (size_t i = k + 1; i < j; i++) {
        top->entries[top->count++] = run->entries[i];
        S_retain_entry(run, i);
    }
    top->entries[top->count++] = S_cut(made, &used, run->entries[j], b + 1, false);
    S_recount_made(made, prefix, used);
    S_recount_made(made, 1, prefix);
    S_recount(top);
    return top;
}

/* Restores half the width along one spine of the tree in `*root` -- the
 * first entries' when `last` is false, the last entries' when it is true --
 * from the top down. A spine run below what it needs joins its neighbour or
 * borrows from it: it needs half the width, and one entry more above tier
 * zero, because the join of a run below it may take one of its entries. A
 * root of one entry gives way to it. False when copying a shared run
 * failed. */
static bool S_normalize(markdown_core_node_pool *pool, markdown_core_run **root, bool last) {
    markdown_core_run **slot = root;
    for (;;) {
        if (!S_unshare(pool, slot)) {
            return false;
        }
        markdown_core_run *run = *slot;
        if (!run->tier) {
            break;
        }
        if (slot == root && run->count == 1) {
            *root = run->entries[0];
            markdown_core_run_free_slot(pool, run);
            continue;
        }
        size_t x = last ? run->count - 1u : 0, y = last ? run->count - 2u : 1;
        if (!S_unshare(pool, (markdown_core_run **)&run->entries[x])) {
            return false;
        }
        markdown_core_run *spine = run->entries[x];
        size_t needed = spine->tier ? S_HALF + 1 : S_HALF;
        if (spine->count < needed) {
            if (!S_unshare(pool, (markdown_core_run **)&run->entries[y])) {
                return false;
            }
            markdown_core_run *other = run->entries[y];
            if (spine->count + other->count <= MARKDOWN_CORE_RUN_WIDTH) {
                /* The two join in the spine run, in order. */
                if (last) {
                    memmove(&spine->entries[other->count], spine->entries, spine->count * sizeof(spine->entries[0]));
                    memcpy(spine->entries, other->entries, other->count * sizeof(other->entries[0]));
                } else {
                    memcpy(&spine->entries[spine->count], other->entries, other->count * sizeof(other->entries[0]));
                }
                spine->count = (uint8_t)(spine->count + other->count);
                S_take(run, y);
                markdown_core_run_free_slot(pool, other);
                x = last ? run->count - 1u : 0;
            } else {
                while (spine->count < needed) {
                    if (last) {
                        S_put(spine, 0, S_take(other, other->count - 1u));
                    } else {
                        S_put(spine, spine->count, S_take(other, 0));
                    }
                }
                S_recount(other);
            }
            S_recount(spine);
            S_recount(run);
        }
        slot = (markdown_core_run **)&run->entries[x];
    }
    /* A join below the root may have left it one entry. */
    while ((*root)->tier && (*root)->count == 1) {
        markdown_core_run *only = (*root)->entries[0];
        markdown_core_run_free_slot(pool, *root);
        *root = only;
    }
    return true;
}

/* Puts `entry`, a run one tier below the last run of `path`, at `at` in that
 * run: a full run on the way up splits in halves, each half then holding at
 * least half the width, and a split root gets a root above it, from `made`. */
static void S_put_up(markdown_core_run **root, markdown_core_run **runs, const size_t *at_entry, int depth, size_t at,
                     void *entry, markdown_core_run **made) {
    int used = 0;
    int tier = depth - 1;
    for (; entry && tier >= 0; tier--) {
        markdown_core_run *run = runs[tier];
        if (run->count < MARKDOWN_CORE_RUN_WIDTH) {
            S_put(run, at, entry);
            entry = NULL;
        } else {
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
            S_recount(right);
            entry = right;
            if (tier) {
                at = at_entry[tier - 1] + 1;
            }
        }
        S_recount(run);
    }
    for (; tier >= 0; tier--) {
        S_recount(runs[tier]);
    }
    if (entry) {
        markdown_core_run *top = made[used];
        top->tier = (uint8_t)(runs[0]->tier + 1);
        top->entries[0] = runs[0];
        top->entries[1] = entry;
        top->count = 2;
        S_recount(top);
        *root = top;
    }
}

bool markdown_core_children_join(markdown_core_node_pool *pool, markdown_core_run **root, markdown_core_run *tail) {
    if (!tail) {
        return true;
    }
    if (!S_normalize(pool, &tail, false)) {
        markdown_core_node_pool_release_children(pool, tail);
        return false;
    }
    if (!*root) {
        *root = tail;
        return true;
    }
    if (!S_normalize(pool, root, true) || !S_unshare(pool, &tail)) {
        markdown_core_node_pool_release_children(pool, tail);
        return false;
    }
    markdown_core_run *left = *root;
    if (left->tier == tail->tier) {
        if (left->count + tail->count <= MARKDOWN_CORE_RUN_WIDTH) {
            memcpy(&left->entries[left->count], tail->entries, tail->count * sizeof(tail->entries[0]));
            left->count = (uint8_t)(left->count + tail->count);
            markdown_core_run_free_slot(pool, tail);
            S_recount(left);
            return true;
        }
        markdown_core_run *top = markdown_core_run_new(pool, (uint8_t)(left->tier + 1));
        if (!top) {
            markdown_core_node_pool_release_children(pool, tail);
            return false;
        }
        while (left->count < S_HALF) {
            S_put(left, left->count, S_take(tail, 0));
        }
        S_recount(left);
        S_recount(tail);
        top->entries[0] = left;
        top->entries[1] = tail;
        top->count = 2;
        S_recount(top);
        *root = top;
        return true;
    }
    /* The taller tree's spine down to the tier above the shorter's root,
     * held by this tree alone, and the runs a split up it may need. */
    bool append = left->tier > tail->tier;
    markdown_core_run **slot = append ? root : &tail;
    uint8_t below = append ? tail->tier : left->tier;
    markdown_core_run *runs[MARKDOWN_CORE_RUN_TIERS];
    size_t at[MARKDOWN_CORE_RUN_TIERS];
    int depth = 0;
    for (;;) {
        if (!S_unshare(pool, slot)) {
            markdown_core_node_pool_release_children(pool, tail);
            return false;
        }
        markdown_core_run *run = *slot;
        runs[depth] = run;
        at[depth] = append ? run->count - 1u : 0;
        depth++;
        if (run->tier == below + 1) {
            break;
        }
        slot = (markdown_core_run **)&run->entries[at[depth - 1]];
    }
    markdown_core_run *made[MARKDOWN_CORE_RUN_TIERS + 1];
    for (int i = 0; i <= depth; i++) {
        if (!(made[i] = markdown_core_run_new(pool, 0))) {
            while (i--) {
                markdown_core_run_free_slot(pool, made[i]);
            }
            markdown_core_node_pool_release_children(pool, tail);
            return false;
        }
    }
    markdown_core_run *parent = runs[depth - 1];
    void *entry = append ? (void *)tail : (void *)left;
    size_t put = append ? parent->count : 0;
    if (!append) {
        /* The left tree's root comes first in a run of the right tree's
         * spine, before a run of at least half the width, and joins it or
         * borrows from it when it holds less. */
        if (!S_unshare(pool, (markdown_core_run **)&parent->entries[0])) {
            for (int i = 0; i <= depth; i++) {
                markdown_core_run_free_slot(pool, made[i]);
            }
            markdown_core_node_pool_release_children(pool, tail);
            return false;
        }
        markdown_core_run *next = parent->entries[0];
        if (left->count < S_HALF) {
            if (left->count + next->count <= MARKDOWN_CORE_RUN_WIDTH) {
                memmove(&next->entries[left->count], next->entries, next->count * sizeof(next->entries[0]));
                memcpy(next->entries, left->entries, left->count * sizeof(left->entries[0]));
                next->count = (uint8_t)(next->count + left->count);
                next->sealed = 0;
                S_recount(next);
                markdown_core_run_free_slot(pool, left);
                entry = NULL;
            } else {
                while (left->count < S_HALF) {
                    S_put(left, left->count, S_take(next, 0));
                }
                S_recount(left);
                S_recount(next);
            }
        }
    }
    int spare = 0;
    if (entry) {
        S_put_up(append ? root : &tail, runs, at, depth, put, entry, made);
        while (spare <= depth && made[spare]->count) {
            spare++;
        }
    } else {
        for (int tier = depth - 1; tier >= 0; tier--) {
            S_recount(runs[tier]);
        }
    }
    for (int i = spare; i <= depth; i++) {
        markdown_core_run_free_slot(pool, made[i]);
    }
    if (!append) {
        *root = tail;
    }
    return true;
}
