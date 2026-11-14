#include "tree.h"
#include <string.h>
#include "alloc.h"

/* THE NODES. A leaf holds its entries; an inner node its children, the
 * entries below each, the shift pending for each, the furthest end below
 * each when the entries have ends (`ends`), and a copy of the first entry
 * below each, in its own terms, which a shift pending for the child has
 * moved already, as it has the furthest end. Every node holds an entry, but the last leaf, which
 * an append may have just made. */
typedef markdown_core_tree_leaf S_leaf;

markdown_core_tree_leaf markdown_core_tree_bare;

typedef struct S_inner {
    uint32_t count;
    struct S_inner *parent;
    void *children[MARKDOWN_CORE_TREE_FANOUT];
    size_t counts[MARKDOWN_CORE_TREE_FANOUT];
    int64_t shifts[MARKDOWN_CORE_TREE_FANOUT];
    size_t ends[MARKDOWN_CORE_TREE_FANOUT];
    uint64_t keys[];
} S_inner;

static inline unsigned char *S_entry(const markdown_core_tree *tree, const S_leaf *leaf, size_t index) {
    return (unsigned char *)leaf->entries + index * tree->ops->size;
}

static inline unsigned char *S_key(const markdown_core_tree *tree, const S_inner *inner, size_t index) {
    return (unsigned char *)inner->keys + index * tree->ops->size;
}

/* `moved` told of the leaf's entries from `from` to `to`. */
static void S_moved(const markdown_core_tree *tree, S_leaf *leaf, uint32_t from, uint32_t to) {
    if (tree->ops->moved) {
        for (uint32_t i = from; i < to; i++) {
            tree->ops->moved(tree, S_entry(tree, leaf, i), leaf, i);
        }
    }
}

/* The parent of `node`, `height` levels above the leaves, set. */
static inline void S_adopted(void *node, uint32_t height, S_inner *parent) {
    if (height) {
        ((S_inner *)node)->parent = parent;
    } else {
        ((S_leaf *)node)->parent = parent;
    }
}

/* A leaf of no entries: its storage holds nothing until an entry is put. */
static S_leaf *S_leaf_new(const markdown_core_tree *tree) {
    S_leaf *leaf = markdown_core_realloc(NULL, sizeof(S_leaf) + MARKDOWN_CORE_TREE_LEAF * tree->ops->size);
    if (leaf) {
        *leaf = (S_leaf){0};
    }
    return leaf;
}

static S_inner *S_inner_new(const markdown_core_tree *tree) {
    return markdown_core_alloc(1, sizeof(S_inner) + MARKDOWN_CORE_TREE_FANOUT * tree->ops->size);
}

/* A walk from the root to a leaf: each inner node and the child it took,
 * the leaf, the entries before it. */
typedef struct S_path {
    S_inner *nodes[MARKDOWN_CORE_TREE_LEVELS];
    uint32_t at[MARKDOWN_CORE_TREE_LEVELS];
    S_leaf *leaf;
    size_t before;
} S_path;

/* The shift pending for child `child` of `inner`, a node `below` levels
 * above the leaves' parents, handed down to the child's entries or its
 * children. */
static void S_open(const markdown_core_tree *tree, S_inner *inner, uint32_t child, bool leaves) {
    const int64_t delta = inner->shifts[child];
    if (!delta) {
        return;
    }
    inner->shifts[child] = 0;
    if (leaves) {
        S_leaf *leaf = inner->children[child];
        for (uint32_t i = 0; i < leaf->count; i++) {
            tree->ops->shift(S_entry(tree, leaf, i), delta);
        }
        return;
    }
    S_inner *node = inner->children[child];
    for (uint32_t i = 0; i < node->count; i++) {
        node->shifts[i] += delta;
        node->ends[i] = (size_t)((int64_t)node->ends[i] + delta);
        tree->ops->shift(S_key(tree, node, i), delta);
    }
}

/* The furthest end below `node`, `height` levels above the leaves, or 0
 * for an empty leaf. */
static size_t S_end(const markdown_core_tree *tree, const void *node, uint32_t height) {
    size_t end = 0;
    if (!height) {
        const S_leaf *leaf = node;
        for (uint32_t i = 0; i < leaf->count; i++) {
            const size_t at = tree->ops->end(S_entry(tree, leaf, i));
            end = at > end ? at : end;
        }
        return end;
    }
    const S_inner *inner = node;
    for (uint32_t i = 0; i < inner->count; i++) {
        end = inner->ends[i] > end ? inner->ends[i] : end;
    }
    return end;
}

/* The first entry below `node`, `level` levels above the leaves, or NULL
 * for an empty leaf. */
static const unsigned char *S_first(const markdown_core_tree *tree, const void *node, uint32_t level) {
    if (!level) {
        const S_leaf *leaf = node;
        return leaf->count ? S_entry(tree, leaf, 0) : NULL;
    }
    return S_key(tree, node, 0);
}

/* The copies of the first entries along `path`, from the leaf up, made
 * again, as far as they change. */
static void S_rekey(const markdown_core_tree *tree, const S_path *path) {
    for (uint32_t level = tree->levels; level--;) {
        S_inner *inner = path->nodes[level];
        const uint32_t child = path->at[level];
        const unsigned char *first = S_first(tree, inner->children[child], tree->levels - level - 1);
        if (first) {
            memcpy(S_key(tree, inner, child), first, tree->ops->size);
        }
        if (child) {
            return;
        }
    }
}

/* The furthest ends along `path`, from the leaf up, made again. */
static void S_reend(const markdown_core_tree *tree, const S_path *path, uint32_t from) {
    if (!tree->ops->end) {
        return;
    }
    for (uint32_t level = from; level--;) {
        S_inner *inner = path->nodes[level];
        const uint32_t child = path->at[level];
        inner->ends[child] = S_end(tree, inner->children[child], tree->levels - level - 1);
    }
}

/* The path to the last leaf, which no shift pending reaches when the tree
 * may append in place. */
static void S_last_path(const markdown_core_tree *tree, S_path *path) {
    void *node = tree->root;
    for (uint32_t level = 0; level < tree->levels; level++) {
        S_inner *inner = node;
        path->nodes[level] = inner;
        path->at[level] = inner->count - 1;
        node = inner->children[inner->count - 1];
    }
    path->leaf = node;
    path->before = tree->count - path->leaf->count;
}

/* The appends in place counted above the last leaf, and the copy of its
 * first entry made when they gave it its first. Before any other change,
 * while no shift is pending above the last leaf. */
static void S_settle(markdown_core_tree *tree) {
    S_leaf *last = tree->last;
    if (!last || last->count == tree->last_held) {
        return;
    }
    const size_t added = last->count - tree->last_held;
    const bool first = !tree->last_held;
    tree->last_held = last->count;
    if (!tree->levels) {
        return;
    }
    size_t end = 0;
    if (tree->ops->end) {
        for (uint32_t i = last->count - (uint32_t)added; i < last->count; i++) {
            const size_t at = tree->ops->end(S_entry(tree, last, i));
            end = at > end ? at : end;
        }
    }
    S_path path;
    void *node = tree->root;
    for (uint32_t level = 0; level < tree->levels; level++) {
        S_inner *inner = node;
        assert(!inner->shifts[inner->count - 1]);
        inner->counts[inner->count - 1] += added;
        if (inner->ends[inner->count - 1] < end) {
            inner->ends[inner->count - 1] = end;
        }
        path.nodes[level] = inner;
        path.at[level] = inner->count - 1;
        node = inner->children[inner->count - 1];
    }
    path.leaf = node;
    if (first) {
        S_rekey(tree, &path);
    }
}

/* The places an append may take in place in the last leaf. */
static void S_spare(markdown_core_tree *tree) {
    tree->spare = 0;
    if (!tree->last || tree->front != tree->count) {
        return;
    }
    const void *node = tree->root;
    for (uint32_t level = 0; level < tree->levels; level++) {
        const S_inner *inner = node;
        if (inner->shifts[inner->count - 1]) {
            return;
        }
        node = inner->children[inner->count - 1];
    }
    tree->spare = MARKDOWN_CORE_TREE_LEAF - tree->last->count;
}

/* The walk to the leaf that holds rank `index`, or the last leaf for the
 * count, handing pending shifts down on the way. */
static void S_descend(const markdown_core_tree *tree, size_t index, S_path *path) {
    void *node = tree->root;
    size_t before = 0;
    for (uint32_t level = 0; level < tree->levels; level++) {
        S_inner *inner = node;
        uint32_t child = 0;
        while (child + 1 < inner->count && index - before >= inner->counts[child]) {
            before += inner->counts[child++];
        }
        S_open(tree, inner, child, level + 1 == tree->levels);
        path->nodes[level] = inner;
        path->at[level] = child;
        node = inner->children[child];
    }
    path->leaf = node;
    path->before = before;
}

void markdown_core_tree_free(markdown_core_tree *tree) {
    if (tree->root) {
        void *stack[MARKDOWN_CORE_TREE_LEVELS + 1];
        uint32_t next[MARKDOWN_CORE_TREE_LEVELS + 1];
        uint32_t top = 0;
        stack[0] = tree->root;
        next[0] = 0;
        for (;;) {
            if (top == tree->levels) {
                markdown_core_free(stack[top]);
                if (!top--) {
                    break;
                }
                continue;
            }
            S_inner *inner = stack[top];
            if (next[top] == inner->count) {
                markdown_core_free(inner);
                if (!top--) {
                    break;
                }
                continue;
            }
            stack[top + 1] = inner->children[next[top]++];
            next[++top] = 0;
        }
    }
    const markdown_core_tree_ops *ops = tree->ops;
    *tree = markdown_core_tree_make(ops);
}

void *markdown_core_tree_find(markdown_core_tree *tree, size_t index) {
    assert(index < tree->count);
    S_settle(tree);
    S_path path;
    S_descend(tree, index, &path);
    tree->seen = path.leaf;
    tree->seen_first = path.before;
    return S_entry(tree, path.leaf, index - path.before);
}

const void *markdown_core_tree_peek(const markdown_core_tree *tree, size_t index) {
    assert(index < tree->count);
    const void *node = tree->root;
    size_t before = 0;
    for (uint32_t level = 0; level < tree->levels; level++) {
        const S_inner *inner = node;
        uint32_t child = 0;
        while (child + 1 < inner->count && index - before >= inner->counts[child]) {
            before += inner->counts[child++];
        }
        node = inner->children[child];
    }
    return S_entry(tree, node, index - before);
}

/* Whether the entry at `entry` comes before `place` or `key`. */
static inline bool S_before(const markdown_core_tree *tree, const unsigned char *entry, size_t place, const void *key) {
    return key ? tree->ops->before(entry, key) : tree->ops->start(entry) < place;
}

static size_t S_from(markdown_core_tree *tree, size_t place, const void *key, size_t low, size_t high) {
    if (!tree->root || low >= high) {
        return low;
    }
    S_settle(tree);
    void *node = tree->root;
    size_t before = 0;
    for (uint32_t level = 0; level < tree->levels; level++) {
        S_inner *inner = node;
        /* The first child that holds an entry of the range, then each later
         * one that starts in it, as long as its first entry is before. */
        uint32_t child = 0;
        while (child + 1 < inner->count && before + inner->counts[child] <= low) {
            before += inner->counts[child++];
        }
        size_t start = before + inner->counts[child];
        for (uint32_t i = child + 1; i < inner->count && start < high && inner->counts[i]; i++) {
            if (!S_before(tree, S_key(tree, inner, i), place, key)) {
                break;
            }
            before = start;
            start += inner->counts[i];
            child = i;
        }
        S_open(tree, inner, child, level + 1 == tree->levels);
        node = inner->children[child];
    }
    S_leaf *leaf = node;
    uint32_t first = low > before ? (uint32_t)(low - before) : 0;
    uint32_t last = high - before < leaf->count ? (uint32_t)(high - before) : leaf->count;
    while (first < last) {
        const uint32_t middle = first + (last - first) / 2;
        if (S_before(tree, S_entry(tree, leaf, middle), place, key)) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    tree->seen = leaf;
    tree->seen_first = before;
    S_spare(tree);
    const size_t found = before + first;
    return found < high ? found : high;
}

size_t markdown_core_tree_from(const markdown_core_tree *tree, size_t place, size_t low, size_t high) {
    return S_from((markdown_core_tree *)tree, place, NULL, low, high);
}

size_t markdown_core_tree_seek(const markdown_core_tree *tree, const void *key) {
    /* A tree of keys moves none, and appends none in place: nothing is
     * pending in it, and the counts above its leaves hold. */
    if (!tree->root) {
        return 0;
    }
    assert(!tree->ops->shift && !tree->spare && tree->last_held == tree->last->count);
    const void *node = tree->root;
    size_t before = 0;
    for (uint32_t level = 0; level < tree->levels; level++) {
        const S_inner *inner = node;
        uint32_t child = 0;
        while (child + 1 < inner->count && inner->counts[child + 1] &&
               tree->ops->before(S_key(tree, inner, child + 1), key)) {
            before += inner->counts[child++];
        }
        node = inner->children[child];
    }
    const S_leaf *leaf = node;
    uint32_t first = 0, last = leaf->count;
    while (first < last) {
        const uint32_t middle = first + (last - first) / 2;
        if (tree->ops->before(S_entry(tree, leaf, middle), key)) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return before + first;
}

/* The first rank of `low` up to `high` below `node`, `height` levels above
 * the leaves, `before` entries before it, whose entry ends past `place`, or
 * SIZE_MAX: a child goes unread when nothing below it ends past it. */
static size_t S_past(markdown_core_tree *tree, void *node, uint32_t height, size_t before, size_t place, size_t low,
                     size_t high) {
    if (!height) {
        S_leaf *leaf = node;
        uint32_t i = low > before ? (uint32_t)(low - before) : 0;
        const uint32_t last = high - before < leaf->count ? (uint32_t)(high - before) : leaf->count;
        for (; i < last; i++) {
            if (tree->ops->end(S_entry(tree, leaf, i)) > place) {
                tree->seen = leaf;
                tree->seen_first = before;
                return before + i;
            }
        }
        return SIZE_MAX;
    }
    S_inner *inner = node;
    for (uint32_t i = 0; i < inner->count && before < high; before += inner->counts[i++]) {
        if (before + inner->counts[i] <= low || inner->ends[i] <= place) {
            continue;
        }
        S_open(tree, inner, i, height == 1);
        const size_t found = S_past(tree, inner->children[i], height - 1, before, place, low, high);
        if (found != SIZE_MAX) {
            return found;
        }
    }
    return SIZE_MAX;
}

size_t markdown_core_tree_past(const markdown_core_tree *tree, size_t place, size_t low, size_t high) {
    assert(tree->ops->end);
    markdown_core_tree *self = (markdown_core_tree *)tree;
    if (!tree->root || low >= high) {
        return high;
    }
    S_settle(self);
    const size_t found = S_past(self, tree->root, tree->levels, 0, place, low, high);
    S_spare(self);
    return found < high ? found : high;
}

/* AN INSERTION goes down by rank, handing pending shifts down, into the
 * leaf, which splits when full, and each full node above it splits in turn
 * as it takes the new node after its child: the node keeps its first half
 * and the new node takes the rest. A root that splits gains a level above
 * it. The nodes the splits make are had first (`room`), so the tree changes
 * only once nothing can fail. */
typedef struct S_put {
    const void *entry;
    void **room;
    uint32_t used;
    /* Where the entry went, and the entries before its leaf. */
    S_leaf *leaf;
    uint32_t k;
    size_t before;
} S_put;

/* The new node after `node` that a split made, or NULL, with its entries in
 * `*made`: the entry put at rank `index` below `node`, `height` levels
 * above the leaves. */
static void *S_put_in(markdown_core_tree *tree, void *node, uint32_t height, size_t index, S_put *put, size_t *made) {
    const size_t size = tree->ops->size;
    if (!height) {
        S_leaf *leaf = node;
        uint32_t k = (uint32_t)index;
        S_leaf *right = NULL;
        if (leaf->count == MARKDOWN_CORE_TREE_LEAF) {
            const uint32_t half = MARKDOWN_CORE_TREE_LEAF / 2;
            right = put->room[put->used++];
            right->count = leaf->count - half;
            memcpy(right->entries, S_entry(tree, leaf, half), right->count * size);
            leaf->count = half;
            right->next = leaf->next;
            right->prev = leaf;
            if (leaf->next) {
                leaf->next->prev = right;
            }
            leaf->next = right;
            right->parent = leaf->parent;
            if (tree->last == leaf) {
                tree->last = right;
            }
            S_moved(tree, right, 0, right->count);
            if (k > half) {
                put->before += half;
                leaf = right;
                k -= half;
            }
        }
        memmove(S_entry(tree, leaf, k + 1), S_entry(tree, leaf, k), (leaf->count - k) * size);
        memcpy(S_entry(tree, leaf, k), put->entry, size);
        leaf->count++;
        S_moved(tree, leaf, k, leaf->count);
        put->leaf = leaf;
        put->k = k;
        *made = right ? right->count : 0;
        return right;
    }
    S_inner *inner = node;
    uint32_t child = 0;
    while (child + 1 < inner->count && index >= inner->counts[child]) {
        index -= inner->counts[child];
        put->before += inner->counts[child++];
    }
    S_open(tree, inner, child, height == 1);
    size_t below = 0;
    void *split = S_put_in(tree, inner->children[child], height - 1, index, put, &below);
    inner->counts[child] += 1 - below;
    memcpy(S_key(tree, inner, child), S_first(tree, inner->children[child], height - 1), size);
    if (tree->ops->end) {
        inner->ends[child] = S_end(tree, inner->children[child], height - 1);
    }
    if (!split) {
        *made = 0;
        return NULL;
    }
    S_inner *into = inner;
    uint32_t slot = child + 1;
    S_inner *right = NULL;
    if (inner->count == MARKDOWN_CORE_TREE_FANOUT) {
        const uint32_t half = MARKDOWN_CORE_TREE_FANOUT / 2;
        right = put->room[put->used++];
        right->count = inner->count - half;
        memcpy(right->children, &inner->children[half], right->count * sizeof(*inner->children));
        memcpy(right->counts, &inner->counts[half], right->count * sizeof(*inner->counts));
        memcpy(right->shifts, &inner->shifts[half], right->count * sizeof(*inner->shifts));
        memcpy(right->ends, &inner->ends[half], right->count * sizeof(*inner->ends));
        memcpy(S_key(tree, right, 0), S_key(tree, inner, half), right->count * size);
        inner->count = half;
        right->parent = inner->parent;
        for (uint32_t i = 0; i < right->count; i++) {
            S_adopted(right->children[i], height - 1, right);
        }
        if (slot > half) {
            into = right;
            slot -= half;
        }
    }
    const uint32_t moved = into->count - slot;
    memmove(&into->children[slot + 1], &into->children[slot], moved * sizeof(*into->children));
    memmove(&into->counts[slot + 1], &into->counts[slot], moved * sizeof(*into->counts));
    memmove(&into->shifts[slot + 1], &into->shifts[slot], moved * sizeof(*into->shifts));
    memmove(&into->ends[slot + 1], &into->ends[slot], moved * sizeof(*into->ends));
    memmove(S_key(tree, into, slot + 1), S_key(tree, into, slot), moved * size);
    into->children[slot] = split;
    S_adopted(split, height - 1, into);
    into->counts[slot] = below;
    into->shifts[slot] = 0;
    into->ends[slot] = tree->ops->end ? S_end(tree, split, height - 1) : 0;
    memcpy(S_key(tree, into, slot), S_first(tree, split, height - 1), size);
    into->count++;
    if (!right) {
        *made = 0;
        return NULL;
    }
    size_t total = 0;
    for (uint32_t i = 0; i < right->count; i++) {
        total += right->counts[i];
    }
    *made = total;
    return right;
}

/* How many nodes the insertion at rank `index` may make: a leaf, an inner
 * node for each full one above it, and a root when every node on the walk is
 * full. */
static uint32_t S_put_room(const markdown_core_tree *tree, size_t index) {
    const void *node = tree->root;
    uint32_t full = 0, levels = 0;
    for (uint32_t level = 0; level < tree->levels; level++) {
        const S_inner *inner = node;
        full = inner->count == MARKDOWN_CORE_TREE_FANOUT ? full + 1 : 0;
        uint32_t child = 0;
        while (child + 1 < inner->count && index >= inner->counts[child]) {
            index -= inner->counts[child++];
        }
        node = inner->children[child];
        levels++;
    }
    if (((const S_leaf *)node)->count < MARKDOWN_CORE_TREE_LEAF) {
        return 0;
    }
    return 1 + full + (full == levels);
}

/* A new root above the old and `split`, which holds `made` entries. */
static void S_raise(markdown_core_tree *tree, void *split, size_t made, S_inner *root) {
    void *old = tree->root;
    root->count = 2;
    root->children[0] = old;
    root->children[1] = split;
    root->parent = NULL;
    S_adopted(old, tree->levels, root);
    S_adopted(split, tree->levels, root);
    root->counts[0] = tree->count - made;
    root->counts[1] = made;
    root->shifts[0] = root->shifts[1] = 0;
    root->ends[0] = tree->ops->end ? S_end(tree, old, tree->levels) : 0;
    root->ends[1] = tree->ops->end ? S_end(tree, split, tree->levels) : 0;
    memcpy(S_key(tree, root, 0), S_first(tree, old, tree->levels), tree->ops->size);
    memcpy(S_key(tree, root, 1), S_first(tree, split, tree->levels), tree->ops->size);
    tree->root = root;
    tree->levels++;
}

void *markdown_core_tree_insert(markdown_core_tree *tree, size_t index, const void *entry, bool front) {
    assert(index <= tree->count);
    if (!tree->root) {
        S_leaf *leaf = S_leaf_new(tree);
        if (!leaf) {
            return NULL;
        }
        tree->root = tree->last = leaf;
        tree->last_held = 0;
    }
    S_settle(tree);
    void *room[MARKDOWN_CORE_TREE_LEVELS + 2];
    const uint32_t needed = S_put_room(tree, index);
    assert(tree->levels + 1 < MARKDOWN_CORE_TREE_LEVELS);
    for (uint32_t i = 0; i < needed; i++) {
        room[i] = i ? (void *)S_inner_new(tree) : (void *)S_leaf_new(tree);
        if (!room[i]) {
            while (i) {
                markdown_core_free(room[--i]);
            }
            return NULL;
        }
    }
    S_put put = {.entry = entry, .room = room};
    size_t made = 0;
    void *split = S_put_in(tree, tree->root, tree->levels, index, &put, &made);
    tree->count++;
    if (split) {
        S_raise(tree, split, made, room[put.used++]);
    }
    assert(put.used == needed);
    tree->last_held = tree->last->count;
    if (index < tree->front || (index == tree->front && front)) {
        tree->front++;
    }
    tree->seen = put.leaf;
    tree->seen_first = put.before;
    S_spare(tree);
    return S_entry(tree, put.leaf, put.k);
}

/* AN APPEND past a full last leaf, or into an empty tree, makes an empty
 * last leaf, which the append fills in place: a full node on the right edge
 * splits as an insertion's would, its new node taking only the new child. */
static void *S_attach(markdown_core_tree *tree, S_inner *inner, uint32_t height, S_leaf *leaf, void **room,
                      uint32_t *used) {
    void *split = NULL;
    const uint32_t last = inner->count - 1;
    S_open(tree, inner, last, height == 1);
    if (height > 1) {
        split = S_attach(tree, inner->children[last], height - 1, leaf, room, used);
        if (!split) {
            return NULL;
        }
    } else {
        split = leaf;
    }
    if (inner->count < MARKDOWN_CORE_TREE_FANOUT) {
        inner->children[inner->count] = split;
        S_adopted(split, height - 1, inner);
        inner->counts[inner->count] = 0;
        inner->shifts[inner->count] = 0;
        inner->ends[inner->count] = 0;
        inner->count++;
        return NULL;
    }
    S_inner *right = room[(*used)++];
    right->count = 1;
    right->children[0] = split;
    right->parent = inner->parent;
    S_adopted(split, height - 1, right);
    right->counts[0] = 0;
    right->shifts[0] = 0;
    right->ends[0] = 0;
    return right;
}

void *markdown_core_tree_append(markdown_core_tree *tree) {
    assert(tree->front == tree->count);
    S_settle(tree);
    S_spare(tree);
    if (tree->spare) {
        return markdown_core_tree_push(tree, tree->ops->size);
    }
    if (tree->last && tree->last->count < MARKDOWN_CORE_TREE_LEAF) {
        /* Room, behind a pending shift: hand it down. */
        S_path path;
        S_last_path(tree, &path);
        for (uint32_t level = 0; level < tree->levels; level++) {
            S_open(tree, path.nodes[level], path.at[level], level + 1 == tree->levels);
        }
        S_spare(tree);
        return markdown_core_tree_push(tree, tree->ops->size);
    }
    S_leaf *leaf = S_leaf_new(tree);
    if (!leaf) {
        return NULL;
    }
    if (!tree->root) {
        tree->root = tree->last = leaf;
    } else {
        /* Nodes for the full ones on the right edge, and a root. */
        void *room[MARKDOWN_CORE_TREE_LEVELS + 1];
        uint32_t needed = 0;
        const void *node = tree->root;
        for (uint32_t level = 0; level < tree->levels; level++) {
            const S_inner *inner = node;
            needed = inner->count == MARKDOWN_CORE_TREE_FANOUT ? needed + 1 : 0;
            node = inner->children[inner->count - 1];
        }
        needed += needed == tree->levels;
        for (uint32_t i = 0; i < needed; i++) {
            room[i] = S_inner_new(tree);
            if (!room[i]) {
                while (i) {
                    markdown_core_free(room[--i]);
                }
                markdown_core_free(leaf);
                return NULL;
            }
        }
        uint32_t used = 0;
        void *split = tree->levels ? S_attach(tree, tree->root, tree->levels, leaf, room, &used) : leaf;
        if (split) {
            S_inner *root = room[used++];
            root->count = 2;
            root->children[0] = tree->root;
            root->children[1] = split;
            root->parent = NULL;
            S_adopted(tree->root, tree->levels, root);
            S_adopted(split, tree->levels, root);
            root->counts[0] = tree->count;
            root->counts[1] = 0;
            root->shifts[0] = root->shifts[1] = 0;
            root->ends[0] = tree->ops->end ? S_end(tree, tree->root, tree->levels) : 0;
            root->ends[1] = 0;
            memcpy(S_key(tree, root, 0), S_first(tree, tree->root, tree->levels), tree->ops->size);
            tree->root = root;
            tree->levels++;
        }
        assert(used == needed);
        leaf->prev = tree->last;
        tree->last->next = leaf;
        tree->last = leaf;
    }
    tree->last_held = 0;
    S_spare(tree);
    assert(tree->spare);
    /* The new leaf is at hand, as a reading of what the append wrote reads
     * it next. */
    tree->seen = leaf;
    tree->seen_first = tree->count;
    return markdown_core_tree_push(tree, tree->ops->size);
}

/* AN ERASURE takes entries out of a leaf at a time. A leaf it leaves empty
 * goes, and so does each node above that its going leaves empty; a node
 * whose first child goes has the copies of its first entry above it made
 * again; a root left with one child gives way to it. */
static void S_drop_leaf(markdown_core_tree *tree, const S_path *path) {
    S_leaf *leaf = path->leaf;
    if (leaf->prev) {
        leaf->prev->next = leaf->next;
    }
    if (leaf->next) {
        leaf->next->prev = leaf->prev;
    }
    if (tree->last == leaf) {
        tree->last = leaf->prev;
    }
    markdown_core_free(leaf);
    uint32_t level = tree->levels;
    for (;;) {
        if (!level) {
            tree->root = NULL;
            tree->levels = 0;
            tree->last = NULL;
            return;
        }
        level--;
        S_inner *inner = path->nodes[level];
        const uint32_t child = path->at[level];
        const uint32_t moved = inner->count - child - 1;
        memmove(&inner->children[child], &inner->children[child + 1], moved * sizeof(*inner->children));
        memmove(&inner->counts[child], &inner->counts[child + 1], moved * sizeof(*inner->counts));
        memmove(&inner->shifts[child], &inner->shifts[child + 1], moved * sizeof(*inner->shifts));
        memmove(&inner->ends[child], &inner->ends[child + 1], moved * sizeof(*inner->ends));
        memmove(S_key(tree, inner, child), S_key(tree, inner, child + 1), moved * tree->ops->size);
        if (--inner->count) {
            S_reend(tree, path, level);
            if (!child) {
                for (uint32_t up = level; up--;) {
                    memcpy(S_key(tree, path->nodes[up], path->at[up]), S_key(tree, path->nodes[up + 1], 0),
                           tree->ops->size);
                    if (path->at[up]) {
                        break;
                    }
                }
            }
            break;
        }
        markdown_core_free(inner);
    }
    while (tree->levels && ((S_inner *)tree->root)->count == 1) {
        S_inner *root = tree->root;
        S_open(tree, root, 0, tree->levels == 1);
        tree->root = root->children[0];
        markdown_core_free(root);
        tree->levels--;
        S_adopted(tree->root, tree->levels, NULL);
    }
}

void markdown_core_tree_erase(markdown_core_tree *tree, size_t index, size_t count) {
    assert(index + count <= tree->count);
    if (!count) {
        return;
    }
    S_settle(tree);
    const size_t size = tree->ops->size;
    const size_t gone = count;
    while (count) {
        S_path path;
        S_descend(tree, index, &path);
        S_leaf *leaf = path.leaf;
        const uint32_t k = (uint32_t)(index - path.before);
        const uint32_t taken = (uint32_t)(count < leaf->count - k ? count : leaf->count - k);
        memmove(S_entry(tree, leaf, k), S_entry(tree, leaf, k + taken), (leaf->count - k - taken) * size);
        leaf->count -= taken;
        S_moved(tree, leaf, k, leaf->count);
        for (uint32_t level = 0; level < tree->levels; level++) {
            path.nodes[level]->counts[path.at[level]] -= taken;
        }
        tree->count -= taken;
        count -= taken;
        if (!leaf->count) {
            S_drop_leaf(tree, &path);
        } else {
            if (!k) {
                S_rekey(tree, &path);
            }
            S_reend(tree, &path, tree->levels);
        }
        tree->last_held = tree->last ? tree->last->count : 0;
    }
    if (index < tree->front) {
        tree->front -= tree->front - index < gone ? tree->front - index : gone;
    }
    tree->seen = &markdown_core_tree_bare;
    S_spare(tree);
}

void markdown_core_tree_shift(markdown_core_tree *tree, size_t index, int64_t delta) {
    if (!delta || index >= tree->count) {
        return;
    }
    S_settle(tree);
    S_path path;
    S_descend(tree, index, &path);
    for (uint32_t level = 0; level < tree->levels; level++) {
        S_inner *inner = path.nodes[level];
        for (uint32_t i = path.at[level] + 1; i < inner->count; i++) {
            inner->shifts[i] += delta;
            inner->ends[i] = (size_t)((int64_t)inner->ends[i] + delta);
            tree->ops->shift(S_key(tree, inner, i), delta);
        }
    }
    S_leaf *leaf = path.leaf;
    const uint32_t k = (uint32_t)(index - path.before);
    for (uint32_t i = k; i < leaf->count; i++) {
        tree->ops->shift(S_entry(tree, leaf, i), delta);
    }
    if (!k) {
        S_rekey(tree, &path);
    }
    S_reend(tree, &path, tree->levels);
    tree->seen = leaf;
    tree->seen_first = path.before;
    S_spare(tree);
}

/* The child slot of `node` in its parent. */
static uint32_t S_slot(const S_inner *parent, const void *node) {
    uint32_t slot = 0;
    while (parent->children[slot] != node) {
        slot++;
    }
    return slot;
}

void markdown_core_tree_read(const markdown_core_tree *tree, markdown_core_tree_handle handle, void *entry) {
    memcpy(entry, S_entry(tree, handle.leaf, handle.slot), tree->ops->size);
    int64_t delta = 0;
    const void *node = handle.leaf;
    for (const S_inner *parent = handle.leaf->parent; parent; node = parent, parent = parent->parent) {
        delta += parent->shifts[S_slot(parent, node)];
    }
    if (delta) {
        tree->ops->shift(entry, delta);
    }
}

markdown_core_tree_handle markdown_core_tree_where(markdown_core_tree *tree, size_t index) {
    (void)markdown_core_tree_find(tree, index);
    return (markdown_core_tree_handle){tree->seen, (uint32_t)(index - tree->seen_first)};
}

size_t markdown_core_tree_rank(const markdown_core_tree *tree, markdown_core_tree_handle handle) {
    size_t rank = handle.slot;
    const void *node = handle.leaf;
    for (const S_inner *parent = handle.leaf->parent; parent; node = parent, parent = parent->parent) {
        const uint32_t slot = S_slot(parent, node);
        for (uint32_t i = 0; i < slot; i++) {
            rank += parent->counts[i];
        }
    }
    (void)tree;
    return rank;
}

size_t markdown_core_tree_room(const markdown_core_tree *tree) {
    size_t room = 0;
    for (const S_leaf *leaf = tree->last; leaf; leaf = leaf->prev) {
        room += MARKDOWN_CORE_TREE_LEAF;
    }
    return room;
}
