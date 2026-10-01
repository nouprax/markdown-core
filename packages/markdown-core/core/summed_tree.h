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
 * checkpoints and their entries (checkpoints.h) are one such sequence.
 *
 * The tree is intrusive: an element embeds a node and the tree never
 * allocates. Each node knows its parent, so the offset of any element is
 * found from the element itself. */

typedef struct markdown_core_summed_node {
    struct markdown_core_summed_node *before, *after, *up;
    /* The element's own measure, and its subtree's sum and height. */
    size_t own, sum;
    int level;
} markdown_core_summed_node;

typedef struct markdown_core_summed_tree {
    markdown_core_summed_node *root;
} markdown_core_summed_tree;

/* Builds a balanced tree of `count` nodes in the order given, their `own`
 * measure set, replacing nothing: the tree must be empty. */
void markdown_core_summed_build(markdown_core_summed_tree *tree, markdown_core_summed_node *const *nodes, size_t count);

/* Inserts `node`, its `own` measure set, right after `at`, or first when
 * `at` is NULL. */
void markdown_core_summed_insert_after(markdown_core_summed_tree *tree, markdown_core_summed_node *at,
                                       markdown_core_summed_node *node);
/* Takes `node` out of the tree. */
void markdown_core_summed_remove(markdown_core_summed_tree *tree, markdown_core_summed_node *node);
/* Takes `node` out of the tree, or puts it in at the absolute `offset`
 * after every element at or before it, keeping where every other element is:
 * the measure of the element after it absorbs the change. */
void markdown_core_summed_take(markdown_core_summed_tree *tree, markdown_core_summed_node *node);
void markdown_core_summed_put(markdown_core_summed_tree *tree, markdown_core_summed_node *node, size_t offset);
/* Recomputes the sums above `node` after its `own` measure changed. */
void markdown_core_summed_refresh(markdown_core_summed_node *node);

markdown_core_summed_node *markdown_core_summed_first(const markdown_core_summed_tree *tree);
markdown_core_summed_node *markdown_core_summed_last(const markdown_core_summed_tree *tree);
markdown_core_summed_node *markdown_core_summed_next(const markdown_core_summed_node *node);
markdown_core_summed_node *markdown_core_summed_previous(const markdown_core_summed_node *node);

/* The sum of the measures of the elements before `node`. */
size_t markdown_core_summed_before(const markdown_core_summed_node *node);
/* The last element whose sum of measures through itself is at most
 * `offset`, or NULL when there is none. */
markdown_core_summed_node *markdown_core_summed_last_through(const markdown_core_summed_tree *tree, size_t offset);

#ifdef __cplusplus
}
#endif

#endif
