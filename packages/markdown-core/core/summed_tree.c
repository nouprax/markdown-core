#include "summed_tree.h"

typedef markdown_core_summed_node node_t;
typedef markdown_core_summed_tree tree_t;

static inline int level_of(const node_t *node) { return node ? node->level : 0; }

static void update(node_t *node) {
    int before = level_of(node->before), after = level_of(node->after);
    node->level = 1 + (before > after ? before : after);
    node->sum = node->own + (node->before ? node->before->sum : 0) + (node->after ? node->after->sum : 0);
}

/* Puts `with` where `node` hangs: under the node's parent, or at the root. */
static void replace(tree_t *tree, node_t *node, node_t *with) {
    node_t *up = node->up;
    if (!up) {
        tree->root = with;
    } else if (up->before == node) {
        up->before = with;
    } else {
        up->after = with;
    }
    if (with) {
        with->up = up;
    }
}

static node_t *lift_before(tree_t *tree, node_t *node) {
    node_t *before = node->before;
    node->before = before->after;
    if (node->before) {
        node->before->up = node;
    }
    replace(tree, node, before);
    before->after = node;
    node->up = before;
    update(node);
    update(before);
    return before;
}

static node_t *lift_after(tree_t *tree, node_t *node) {
    node_t *after = node->after;
    node->after = after->before;
    if (node->after) {
        node->after->up = node;
    }
    replace(tree, node, after);
    after->before = node;
    node->up = after;
    update(node);
    update(after);
    return after;
}

/* Every subtree from `node` to the root, balanced again after one insertion
 * or removal below it, with its sums current. */
static void rebalance_up(tree_t *tree, node_t *node) {
    while (node) {
        update(node);
        int balance = level_of(node->before) - level_of(node->after);
        if (balance > 1) {
            if (level_of(node->before->before) < level_of(node->before->after)) {
                lift_after(tree, node->before);
            }
            node = lift_before(tree, node);
        } else if (balance < -1) {
            if (level_of(node->after->after) < level_of(node->after->before)) {
                lift_before(tree, node->after);
            }
            node = lift_after(tree, node);
        }
        node = node->up;
    }
}

static node_t *first_below(node_t *node) {
    while (node->before) {
        node = node->before;
    }
    return node;
}

static node_t *last_below(node_t *node) {
    while (node->after) {
        node = node->after;
    }
    return node;
}

/* A range of the nodes still to place, the slot its middle goes to, the sum
 * of the measures before it, and its height. */
typedef struct {
    size_t from, to, base;
    node_t **slot, *up;
    int level;
} summed_span;

/* The height of a range of `size` nodes, one of the two halves of a range
 * `level` high: a range of n nodes split at its middle is as high as n has
 * binary digits. */
static inline int half_level(size_t size, int level) { return size >> (level - 2) ? level - 1 : level - 2; }

void markdown_core_summed_build(tree_t *tree, node_t *const *nodes, size_t count) {
    /* The middles are placed from a stack of the ranges still to place,
     * with their sums and heights: a range's sum is the sum of the measures
     * through its last node less those before it, so each node's `sum`
     * holds the measures through it until the walk places it. A balanced
     * tree of fewer than 2^64 nodes is under 64 levels high. */
    tree->root = NULL;
    if (!count) {
        return;
    }
    size_t through = 0;
    for (size_t i = 0; i < count; i++) {
        through += nodes[i]->own;
        nodes[i]->sum = through;
    }
    int level = 0;
    for (size_t size = count; size; size >>= 1) {
        level++;
    }
    summed_span stack[130];
    size_t depth = 0;
    stack[depth++] = (summed_span){0, count, 0, &tree->root, NULL, level};
    while (depth) {
        summed_span range = stack[--depth];
        size_t middle = range.from + (range.to - range.from) / 2;
        node_t *node = nodes[middle];
        size_t last = nodes[range.to - 1]->sum, past = node->sum;
        node->sum = last - range.base;
        node->level = range.level;
        node->up = range.up;
        node->before = node->after = NULL;
        *range.slot = node;
        if (middle + 1 < range.to) {
            stack[depth++] = (summed_span){middle + 1,   range.to, past,
                                           &node->after, node,     half_level(range.to - middle - 1, range.level)};
        }
        if (range.from < middle) {
            stack[depth++] = (summed_span){range.from,    middle, range.base,
                                           &node->before, node,   half_level(middle - range.from, range.level)};
        }
    }
}

void markdown_core_summed_insert_after(tree_t *tree, node_t *at, node_t *node) {
    node->before = node->after = NULL;
    node_t *up;
    if (!tree->root) {
        node->up = NULL;
        tree->root = node;
        update(node);
        return;
    }
    if (!at) {
        up = first_below(tree->root);
        up->before = node;
    } else if (!at->after) {
        up = at;
        up->after = node;
    } else {
        up = first_below(at->after);
        up->before = node;
    }
    node->up = up;
    update(node);
    rebalance_up(tree, up);
}

void markdown_core_summed_remove(tree_t *tree, node_t *node) {
    node_t *from;
    if (!node->before || !node->after) {
        from = node->up;
        replace(tree, node, node->before ? node->before : node->after);
    } else {
        /* The next element, the first of the after subtree, takes its place. */
        node_t *next = first_below(node->after);
        if (next->up != node) {
            from = next->up;
            replace(tree, next, next->after);
            next->after = node->after;
            next->after->up = next;
        } else {
            from = next;
        }
        next->before = node->before;
        next->before->up = next;
        replace(tree, node, next);
    }
    node->before = node->after = node->up = NULL;
    rebalance_up(tree, from);
}

void markdown_core_summed_take(tree_t *tree, node_t *node) {
    node_t *next = markdown_core_summed_next(node);
    size_t own = node->own;
    markdown_core_summed_remove(tree, node);
    if (next) {
        next->own += own;
        markdown_core_summed_refresh(next);
    }
}

void markdown_core_summed_put(tree_t *tree, node_t *node, size_t offset) {
    node_t *at = markdown_core_summed_last_through(tree, offset);
    node_t *next = at ? markdown_core_summed_next(at) : markdown_core_summed_first(tree);
    node->own = offset - (at ? markdown_core_summed_before(at) + at->own : 0);
    markdown_core_summed_insert_after(tree, at, node);
    if (next) {
        next->own -= node->own;
        markdown_core_summed_refresh(next);
    }
}

void markdown_core_summed_refresh(node_t *node) {
    for (; node; node = node->up) {
        update(node);
    }
}

node_t *markdown_core_summed_first(const tree_t *tree) { return tree->root ? first_below(tree->root) : NULL; }

node_t *markdown_core_summed_last(const tree_t *tree) { return tree->root ? last_below(tree->root) : NULL; }

node_t *markdown_core_summed_next(const node_t *node) {
    if (node->after) {
        return first_below(node->after);
    }
    while (node->up && node == node->up->after) {
        node = node->up;
    }
    return node->up;
}

node_t *markdown_core_summed_previous(const node_t *node) {
    if (node->before) {
        return last_below(node->before);
    }
    while (node->up && node == node->up->before) {
        node = node->up;
    }
    return node->up;
}

size_t markdown_core_summed_before(const node_t *node) {
    size_t sum = node->before ? node->before->sum : 0;
    for (; node->up; node = node->up) {
        if (node == node->up->after) {
            sum += node->up->own + (node->up->before ? node->up->before->sum : 0);
        }
    }
    return sum;
}

node_t *markdown_core_summed_last_through(const tree_t *tree, size_t offset) {
    node_t *node = tree->root, *found = NULL;
    size_t base = 0;
    while (node) {
        size_t through = base + (node->before ? node->before->sum : 0) + node->own;
        if (through <= offset) {
            found = node;
            base = through;
            node = node->after;
        } else {
            node = node->before;
        }
    }
    return found;
}
