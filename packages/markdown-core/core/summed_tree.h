#ifndef MARKDOWN_CORE_SUMMED_TREE_H
#define MARKDOWN_CORE_SUMMED_TREE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A SEQUENCE WHOSE ELEMENTS HAVE LENGTHS (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.1): a balanced tree in sequence order whose elements store
 * their own measures and whose subtrees store the sums. Absolute offsets are
 * prefix sums, so finding an offset, inserting, removing and shifting
 * everything after a change cost O(log n) wherever the change is. A session's
 * block records (block_records.h) are one such sequence.
 *
 * The tree is intrusive: an element embeds a node and the tree never
 * allocates. Each node knows its parent, so the offset of any element is
 * found from the element itself. */

#define MARKDOWN_CORE_SUMMED_MEASURES 2

typedef struct markdown_core_summed_node {
    struct markdown_core_summed_node *before, *after, *up;
    /* The element's own measures, and its subtree's sums and element count. */
    size_t own[MARKDOWN_CORE_SUMMED_MEASURES];
    size_t sum[MARKDOWN_CORE_SUMMED_MEASURES];
    size_t count;
    int level;
} markdown_core_summed_node;

typedef struct markdown_core_summed_tree {
    markdown_core_summed_node *root;
} markdown_core_summed_tree;

static inline size_t markdown_core_summed_count(const markdown_core_summed_tree *tree) {
    return tree->root ? tree->root->count : 0;
}
static inline size_t markdown_core_summed_total(const markdown_core_summed_tree *tree, int measure) {
    return tree->root ? tree->root->sum[measure] : 0;
}

/* Builds a balanced tree of `count` nodes in the order given, their `own`
 * measures set, replacing nothing: the tree must be empty. */
void markdown_core_summed_build(markdown_core_summed_tree *tree, markdown_core_summed_node *const *nodes,
                                size_t count);

/* Inserts `node`, its `own` measures set, right after `at`, or first when
 * `at` is NULL. */
void markdown_core_summed_insert_after(markdown_core_summed_tree *tree, markdown_core_summed_node *at,
                                       markdown_core_summed_node *node);
/* Takes `node` out of the tree. */
void markdown_core_summed_remove(markdown_core_summed_tree *tree, markdown_core_summed_node *node);
/* Recomputes the sums above `node` after its `own` measures changed. */
void markdown_core_summed_refresh(markdown_core_summed_node *node);

markdown_core_summed_node *markdown_core_summed_first(const markdown_core_summed_tree *tree);
markdown_core_summed_node *markdown_core_summed_last(const markdown_core_summed_tree *tree);
markdown_core_summed_node *markdown_core_summed_next(const markdown_core_summed_node *node);
markdown_core_summed_node *markdown_core_summed_previous(const markdown_core_summed_node *node);

/* The sum of `measure` over the elements before `node`. */
size_t markdown_core_summed_before(const markdown_core_summed_node *node, int measure);
/* The last element whose sum of `measure` through itself is at most
 * `offset`, or NULL when there is none. */
markdown_core_summed_node *markdown_core_summed_last_through(const markdown_core_summed_tree *tree, int measure,
                                                             size_t offset);

#ifdef __cplusplus
}
#endif

#endif
