#include "summed_tree.h"

typedef markdown_core_summed_node node_t;
typedef markdown_core_summed_tree tree_t;

static inline int level_of(const node_t *node) { return node ? node->level : 0; }

static void update(node_t *node) {
    int before = level_of(node->before), after = level_of(node->after);
    node->level = 1 + (before > after ? before : after);
    node->count = 1 + (node->before ? node->before->count : 0) + (node->after ? node->after->count : 0);
    for (int k = 0; k < MARKDOWN_CORE_SUMMED_MEASURES; k++) {
        node->sum[k] =
            node->own[k] + (node->before ? node->before->sum[k] : 0) + (node->after ? node->after->sum[k] : 0);
    }
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

/* A range of the nodes still to place, and the slot its middle goes to. */
typedef struct {
    size_t from, to;
    node_t **slot, *up;
} summed_span;

void markdown_core_summed_build(tree_t *tree, node_t *const *nodes, size_t count) {
    /* Middles first, from a stack of the ranges still to place: a balanced
     * tree of fewer than 2^64 nodes is under 64 levels high. Then the sums,
     * children before parents, in a post-order walk along the parents. */
    summed_span stack[130];
    size_t depth = 0;
    tree->root = NULL;
    stack[depth++] = (summed_span){0, count, &tree->root, NULL};
    while (depth) {
        summed_span range = stack[--depth];
        if (range.from == range.to) {
            *range.slot = NULL;
            continue;
        }
        size_t middle = range.from + (range.to - range.from) / 2;
        node_t *node = nodes[middle];
        *range.slot = node;
        node->up = range.up;
        stack[depth++] = (summed_span){middle + 1, range.to, &node->after, node};
        stack[depth++] = (summed_span){range.from, middle, &node->before, node};
    }
    node_t *node = tree->root;
    if (!node) {
        return;
    }
    for (;;) {
        while (node->before || node->after) {
            node = node->before ? node->before : node->after;
        }
        for (;;) {
            update(node);
            node_t *up = node->up;
            if (!up) {
                return;
            }
            if (node == up->before && up->after) {
                node = up->after;
                break;
            }
            node = up;
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

size_t markdown_core_summed_before(const node_t *node, int measure) {
    size_t sum = node->before ? node->before->sum[measure] : 0;
    for (; node->up; node = node->up) {
        if (node == node->up->after) {
            sum += node->up->own[measure] + (node->up->before ? node->up->before->sum[measure] : 0);
        }
    }
    return sum;
}

node_t *markdown_core_summed_last_through(const tree_t *tree, int measure, size_t offset) {
    node_t *node = tree->root, *found = NULL;
    size_t base = 0;
    while (node) {
        size_t through = base + (node->before ? node->before->sum[measure] : 0) + node->own[measure];
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
