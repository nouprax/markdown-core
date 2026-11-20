#ifndef MARKDOWN_CORE_TREE_H
#define MARKDOWN_CORE_TREE_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A TREE (docs/plans/completion-tail.md, step 1): entries of one size in an
 * order, in leaves under inner nodes that count the entries below each
 * child and hold a copy of the first entry below it, so that the entry of a
 * rank, the first entry in order after a place or a key, an insertion and an
 * erasure each take time logarithmic in the entries, wherever they are.
 *
 * The order is where each entry starts in a text (`start`), or a key's
 * (`before`). Entries that hold places in a text move with an edit before
 * them: `markdown_core_tree_shift` moves every entry from a rank on by a
 * delta, which an inner node keeps PENDING for a child it moves whole and
 * hands down when a reading or a change goes below it, so that an edit moves
 * no entry it does not reach. An entry read through the tree holds its
 * places where they are in the text.
 *
 * The tree has a FOCUS, a rank that parts it in two: the FRONT, the entries
 * before it, which a parse appends to, and the BACK, those from it on, which
 * an edit put back and a parse reads on from. Moving the focus moves no
 * entry.
 *
 * The leaf a reader read last stays at hand (`seen`), and so does the room in
 * the last leaf while the front is the whole tree (`spare`), so that a parse
 * that reads near where it reads and appends at the end reads and writes
 * the leaf in place, as it would an array. An entry's storage holds until
 * the next change to the tree; a write in place may change anything but the
 * entry's order. */
#define MARKDOWN_CORE_TREE_LEAF 64
#define MARKDOWN_CORE_TREE_FANOUT 16
#define MARKDOWN_CORE_TREE_LEVELS 16

/* What a tree's entries are (the interface): their size, their order, and
 * how one moves in the text. `start` orders a tree of places and `before` a
 * tree of keys; the other is NULL, and so is `shift` for entries that hold
 * no places. */
struct markdown_core_tree_leaf;
struct markdown_core_tree;

typedef struct markdown_core_tree_ops {
    size_t size;
    size_t (*start)(const void *entry);
    bool (*before)(const void *a, const void *b);
    void (*shift)(void *entry, int64_t delta);
    /* Called when the entry at `entry` came to be there, slot `slot` of
     * `leaf`, by an insertion, or as its leaf's entries moved: an entry
     * that something outside the tree holds a handle of tells it. NULL for
     * entries nothing holds. The tree is the one that holds the entry. */
    void (*moved)(const struct markdown_core_tree *tree, void *entry, struct markdown_core_tree_leaf *leaf,
                  uint32_t slot);
    /* Where the entry ends in the text, for a tree whose inner nodes keep
     * the furthest end below each child (markdown_core_tree_past), which a
     * shift moves as it moves the entry; NULL for one that keeps none. Such
     * a tree's entries are not written in place but to end further, which
     * markdown_core_tree_raise then tells it. */
    size_t (*end)(const void *entry);
} markdown_core_tree_ops;

typedef struct markdown_core_tree_leaf {
    uint32_t count;
    struct markdown_core_tree_leaf *next, *prev;
    /* The inner node above it, or NULL at the root. */
    void *parent;
    uint64_t entries[];
} markdown_core_tree_leaf;

typedef struct markdown_core_tree {
    void *root;
    /* The levels of inner nodes above the leaves. */
    uint32_t levels;
    size_t count, front;
    const markdown_core_tree_ops *ops;
    /* The last leaf, and how many of its entries the counts above it hold:
     * an append in place counts only in the leaf, and the next change counts
     * it above. */
    markdown_core_tree_leaf *last;
    uint32_t last_held;
    /* The places an append may take in the last leaf in place: none while
     * the tree has a back, or a shift is pending above the last leaf. */
    uint32_t spare;
    /* The slot in the leaf at hand that a seek of a place found last: the
     * next seek starts there. */
    uint32_t found;
    /* The leaf read or appended to last, with no shift pending above it,
     * or a leaf of no entries (markdown_core_tree_bare), and the rank of its
     * first entry. */
    markdown_core_tree_leaf *seen;
    size_t seen_first;
} markdown_core_tree;

/* The leaf at hand of a tree that has none. Never written: a reader of it
 * finds no entries, and every change to a tree puts another leaf at hand
 * first. */
extern const markdown_core_tree_leaf markdown_core_tree_bare;

/* An empty tree of entries `ops` describes. */
static inline markdown_core_tree markdown_core_tree_make(const markdown_core_tree_ops *ops) {
    markdown_core_tree tree = {0};
    tree.ops = ops;
    tree.seen = (markdown_core_tree_leaf *)&markdown_core_tree_bare;
    return tree;
}

void markdown_core_tree_free(markdown_core_tree *tree);

/* The entries the tree's leaves have room for. */
size_t markdown_core_tree_room(const markdown_core_tree *tree);

/* Entry `index`, below the count, its places where they are in the text:
 * the leaf that holds it becomes the one at hand. */
void *markdown_core_tree_find(markdown_core_tree *tree, size_t index);

/* Entry `index`, below the count, read with no change to the tree, which
 * any number of readers may do at once: its places as they were before any
 * shift still pending above it, so a reader reads of it what no shift moves. */
const void *markdown_core_tree_peek(const markdown_core_tree *tree, size_t index);

/* `entry` put at rank `index`, at most the count, the entries from there on
 * after it; the focus moves past it when it was past `index`, or at it and
 * `front` says the entry joins the front. The entry's storage, or NULL, with
 * the tree as it was, when storage could not be had. */
void *markdown_core_tree_insert(markdown_core_tree *tree, size_t index, const void *entry, bool front);

/* The `count` entries from rank `index` go; the focus keeps its place among
 * the others. */
void markdown_core_tree_cut(markdown_core_tree *tree, size_t index, size_t count);
static inline void markdown_core_tree_erase(markdown_core_tree *tree, size_t index, size_t count) {
    /* The last entries, appended in place and counted nowhere else, go
     * from the last leaf alone, which stays open to appends. */
    if (tree->spare && index + count == tree->count && count <= tree->last->count - tree->last_held) {
        tree->last->count -= (uint32_t)count;
        tree->count = tree->front = index;
        tree->spare += (uint32_t)count;
        return;
    }
    if (count) {
        markdown_core_tree_cut(tree, index, count);
    }
}

/* Every entry from rank `index` on moves by `delta` in the text. */
void markdown_core_tree_shift(markdown_core_tree *tree, size_t index, int64_t delta);

/* The rank of the first entry of ranks `low` up to `high` that starts at or
 * past `place`, or `high` (a tree of places, whose entries are in order in
 * that range: the front's and the back's each are, in their own terms); or
 * of the first entry whose key is not before `key`'s, or the count (a tree
 * of keys, which a seek reads with no change to it: any number of readers
 * may seek at once). */
static inline size_t markdown_core_tree_bound(const markdown_core_tree *tree, size_t place, size_t low, size_t high);
static inline size_t markdown_core_tree_from(const markdown_core_tree *tree, size_t place, size_t low, size_t high) {
    return low < high ? markdown_core_tree_bound(tree, place, low, high) : low;
}
size_t markdown_core_tree_seek(const markdown_core_tree *tree, const void *key);

/* The rank of the first entry of ranks `low` up to `high` that ends past
 * `place`, or `high` (a tree whose ops give `end`). */
size_t markdown_core_tree_past(const markdown_core_tree *tree, size_t place, size_t low, size_t high);

/* The furthest end of all the entries, or 0 for none (a tree whose ops
 * give `end`). */
size_t markdown_core_tree_furthest(const markdown_core_tree *tree);

/* Entry `index`, written in place to end further than it did (a tree whose
 * ops give `end`): the furthest ends above it are raised to its end. An
 * entry an append wrote that the tree has not counted above needs none. */
void markdown_core_tree_raise(markdown_core_tree *tree, size_t index);

/* A HANDLE: where an entry is, as its `moved` last said, which holds until
 * the next `moved` of the entry. The entry it holds, its places where they
 * are in the text, into `*entry`, and its rank. */
typedef struct markdown_core_tree_handle {
    markdown_core_tree_leaf *leaf;
    uint32_t slot;
} markdown_core_tree_handle;

void markdown_core_tree_read(const markdown_core_tree *tree, markdown_core_tree_handle handle, void *entry);
/* The handle of entry `index`, below the count, as `moved` last said. */
markdown_core_tree_handle markdown_core_tree_where(markdown_core_tree *tree, size_t index);
size_t markdown_core_tree_rank(const markdown_core_tree *tree, markdown_core_tree_handle handle);

/* The focus moved to rank `index`. */
static inline void markdown_core_tree_focus(markdown_core_tree *tree, size_t index) {
    assert(index <= tree->count);
    tree->front = index;
    tree->spare = 0;
}

/* Entry `index`'s storage: in place when it is in the leaf at hand. */
/* A reading through a tree it may not change changes only what the tree
 * keeps at hand, and hands pending shifts down, which change no entry it
 * reads: the tree is the same tree. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void *markdown_core_tree_at(const markdown_core_tree *tree,
                                                                                   size_t index, size_t size) {
    const size_t k = index - tree->seen_first;
    if (k < tree->seen->count) {
        return (unsigned char *)tree->seen->entries + k * size;
    }
    return markdown_core_tree_find((markdown_core_tree *)tree, index);
}

/* The bound read from the root, which puts the leaf it ends in at hand. */
size_t markdown_core_tree_bound_root(markdown_core_tree *tree, size_t place, size_t low, size_t high);

/* The leaf read last answers when the first entry not before the place is
 * in it, or just past it: what is before the part of the range in it is
 * before the place, and what is past it is not. The search starts from the
 * slot the last seek found, and steps out from there by doubling strides,
 * so a reader that seeks near where it sought last reads a few entries, and
 * any other a logarithm of the leaf; else the tree is read from the root. */
static inline size_t markdown_core_tree_bound(const markdown_core_tree *tree, size_t place, size_t low, size_t high) {
    const markdown_core_tree_leaf *seen = tree->seen;
    const size_t size = tree->ops->size;
    size_t (*const start)(const void *entry) = tree->ops->start;
    const unsigned char *entries = (const unsigned char *)seen->entries;
    const size_t base = tree->seen_first;
    const size_t seen_low = low > base ? low : base;
    const size_t seen_high = high < base + seen->count ? high : base + seen->count;
    if (seen_low < seen_high) {
        /* The answer is in [first, last]. It is the answer of the whole
         * range unless it is an end of the leaf's part that is not an end of
         * the range: then entries of another leaf decide. */
        size_t at = base + tree->found;
        at = at < seen_low ? seen_low : at >= seen_high ? seen_high - 1 : at;
        size_t first, last, step = 1;
        if (start(entries + (at - base) * size) < place) {
            first = at + 1;
            while (first + step <= seen_high && start(entries + (first + step - 1 - base) * size) < place) {
                first += step;
                step *= 2;
            }
            last = first + step <= seen_high ? first + step - 1 : seen_high;
        } else {
            last = at;
            while (last >= seen_low + step && start(entries + (last - step - base) * size) >= place) {
                last -= step;
                step *= 2;
            }
            first = last >= seen_low + step ? last - step + 1 : seen_low;
        }
        while (first < last) {
            const size_t middle = first + (last - first) / 2;
            if (start(entries + (middle - base) * size) < place) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        if ((first < seen_high || seen_high == high) && (first > seen_low || seen_low == low)) {
            ((markdown_core_tree *)tree)->found = (uint32_t)(first - base);
            return first;
        }
    }
    return markdown_core_tree_bound_root((markdown_core_tree *)tree, place, low, high);
}

/* Entry `index`'s storage, as markdown_core_tree_at gives it, and in `*run`
 * how many entries from it on lie after it in that storage: a reader that
 * walks the ranks in order steps through them as through an array. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void *markdown_core_tree_run(const markdown_core_tree *tree,
                                                                                    size_t index, size_t size,
                                                                                    size_t *run) {
    void *entry = markdown_core_tree_at(tree, index, size);
    *run = tree->seen->count - (index - tree->seen_first);
    return entry;
}

/* Entry `index`'s storage, as markdown_core_tree_at gives it, and in
 * `*behind` how many entries before it lie before it in that storage: a
 * reader that walks the ranks down steps through them as through an array. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void *markdown_core_tree_back(const markdown_core_tree *tree,
                                                                                     size_t index, size_t size,
                                                                                     size_t *behind) {
    void *entry = markdown_core_tree_at(tree, index, size);
    *behind = index - tree->seen_first;
    return entry;
}

/* Storage for an entry appended at the end of a tree with no back, which
 * the caller writes: in place while the last leaf has room. NULL when
 * storage could not be had. */
void *markdown_core_tree_append(markdown_core_tree *tree);
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void *markdown_core_tree_push(markdown_core_tree *tree,
                                                                                     size_t size) {
    assert(tree->front == tree->count);
    if (tree->spare) {
        markdown_core_tree_leaf *last = tree->last;
        tree->spare--;
        tree->count++;
        tree->front++;
        return (unsigned char *)last->entries + last->count++ * size;
    }
    return markdown_core_tree_append(tree);
}

/* The typed operations of a tree of entries of `type`, named `name_...`:
 * each a function of its own type over the generic ones above. */
#define MARKDOWN_CORE_TREE_OPS(name, type)                                                                             \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused, always_inline)) type *name##_at(markdown_core_tree *t,              \
                                                                                   size_t index) {                     \
        return (type *)markdown_core_tree_at(t, index, sizeof(type));                                                  \
    }                                                                                                                  \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused, always_inline)) type *name##_push(markdown_core_tree *t) {          \
        return (type *)markdown_core_tree_push(t, sizeof(type));                                                       \
    }                                                                                                                  \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) type *name##_insert(markdown_core_tree *t, size_t index,           \
                                                                        type entry, bool front) {                      \
        return (type *)markdown_core_tree_insert(t, index, &entry, front);                                             \
    }

#ifdef __cplusplus
}
#endif

#endif
