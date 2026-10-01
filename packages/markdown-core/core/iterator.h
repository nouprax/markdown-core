#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stdbool.h>

#include "node_type.h"
#include "markdown-core-element-api.h"
#include "buffer.h"
#include "node.h"

/* A depth-first walk over a subtree: every node yields exactly one ENTER and
 * one EXIT, with its descendants' events between them, and DONE follows the
 * root's EXIT. Node-valued fields are independent roots and are never
 * discovered by this traversal.
 *
 * A node has no parent and no siblings (node.h), so the walk carries its own
 * PATH: one frame per node from the root down to the current one, each frame
 * holding its node and the index of the child the walk is inside. A frame's
 * node is read again for its next child when the walk comes back to it, so a
 * change the walk makes to the children of a node on its path, through the
 * operations below, is what the walk then sees.
 *
 * Walks that nest -- the finish walk enters a field root before the rest of
 * its owner -- share one path: an inner walk's frames sit above the outer
 * one's from its `base`, and are gone when the inner walk is DONE. */
typedef struct {
    markdown_core_node *node;
    /* The index of the child the walk is inside, among the node's children. */
    size_t at;
} markdown_core_iter_frame;

typedef struct {
    markdown_core_iter_frame *frames;
    size_t count, capacity;
} markdown_core_iter_path;

typedef struct markdown_core_iter {
    markdown_core_iter_path *path;
    /* The frames below this walk's root belong to the walks it is inside. */
    size_t base;
    markdown_core_node *root;
    /* The node of the current event: the last frame's, NULL before the
     * first event and once the walk is DONE. */
    markdown_core_node *node;
    markdown_core_event_type event;
    /* The path could not grow: the walk ended early with DONE. */
    bool failed;
    /* The path of an iterator made by markdown_core_iter_new. */
    markdown_core_iter_path own;
} markdown_core_iter;

/* Grows `path` to hold one more frame. */
bool markdown_core_iter_path_reserve(markdown_core_iter_path *path);
void markdown_core_iter_path_dispose(markdown_core_iter_path *path);

/* THE ITERATOR'S STEP, IN THE HEADER. The finish walk takes one per event of
 * every node of every root, and it keeps its iterators in its own frames
 * rather than behind an allocation, so the step is here, where the walk can
 * keep the state in registers instead of calling across a translation unit
 * for it: `markdown_core_iter_next` is this behind a call, and
 * `markdown_core_iter_new` is `markdown_core_iter_init` on a heap iterator
 * with a path of its own. */
static inline void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_iter_path *path,
                                           markdown_core_node *root) {
    iter->path = path;
    iter->base = path->count;
    iter->root = root;
    iter->node = NULL;
    iter->event = MARKDOWN_CORE_EVENT_NONE;
    iter->failed = false;
}

static inline bool markdown_core_iter_push(markdown_core_iter *iter, markdown_core_node *node) {
    markdown_core_iter_path *path = iter->path;
    if (path->count == path->capacity && !markdown_core_iter_path_reserve(path)) {
        path->count = iter->base;
        iter->failed = true;
        iter->node = NULL;
        iter->event = MARKDOWN_CORE_EVENT_DONE;
        return false;
    }
    path->frames[path->count++] = (markdown_core_iter_frame){node, 0};
    iter->node = node;
    return true;
}

static inline markdown_core_event_type markdown_core_iter_step(markdown_core_iter *iter) {
    markdown_core_iter_path *path = iter->path;
    if (iter->event == MARKDOWN_CORE_EVENT_ENTER) {
        const markdown_core_run *children = iter->node->children;
        if (!children) {
            iter->event = MARKDOWN_CORE_EVENT_EXIT;
        } else {
            /* The node's frame was pushed inside its first child. */
            markdown_core_iter_push(iter, markdown_core_children_at(children, 0));
        }
    } else if (iter->event == MARKDOWN_CORE_EVENT_EXIT) {
        if (--path->count == iter->base) {
            iter->node = NULL;
            iter->event = MARKDOWN_CORE_EVENT_DONE;
        } else {
            markdown_core_iter_frame *parent = &path->frames[path->count - 1];
            const markdown_core_run *children = parent->node->children;
            if (++parent->at < markdown_core_children_count(children)) {
                iter->event = MARKDOWN_CORE_EVENT_ENTER;
                markdown_core_iter_push(iter, markdown_core_children_at(children, parent->at));
            } else {
                iter->node = parent->node;
            }
        }
    } else if (iter->event == MARKDOWN_CORE_EVENT_NONE && markdown_core_iter_push(iter, iter->root)) {
        iter->event = MARKDOWN_CORE_EVENT_ENTER;
    }
    return iter->event;
}

/* The node of the current event, or NULL once the walk is DONE. */
static inline markdown_core_node *markdown_core_iter_node(const markdown_core_iter *iter) { return iter->node; }

/* The parent of the current node, or NULL at the root. */
static inline markdown_core_node *markdown_core_iter_parent(const markdown_core_iter *iter) {
    size_t count = iter->path->count;
    return count - iter->base > 1 ? iter->path->frames[count - 2].node : NULL;
}

/* The current node's index among its parent's children; the root has none. */
static inline size_t markdown_core_iter_index(const markdown_core_iter *iter) {
    return iter->path->frames[iter->path->count - 2].at;
}

/* At an ENTER: the walk goes to the node's EXIT without entering its
 * children. */
static inline void markdown_core_iter_skip(markdown_core_iter *iter) {
    assert(iter->event == MARKDOWN_CORE_EVENT_ENTER);
    iter->event = MARKDOWN_CORE_EVENT_EXIT;
}

/* THE CHANGES A WALK MAKES TO THE CURRENT NODE'S PLACE, each keeping the walk
 * on the same path. The current node is not the root. */

/* Puts `node` among the current node's siblings just before it, taking the
 * caller's hold; the walk does not visit it. False, with nothing changed,
 * when storage runs out. */
bool markdown_core_iter_insert_before(markdown_core_iter *iter, markdown_core_node_pool *pool,
                                      markdown_core_node *node);
/* At an EXIT: takes the current node out of its parent and hands the hold to
 * the caller; the walk goes on with what was its next sibling. False, with
 * nothing changed, when storage runs out. */
bool markdown_core_iter_take_current(markdown_core_iter *iter, markdown_core_node_pool *pool,
                                     markdown_core_node **taken);
/* At an EXIT: puts `node` in the current node's place, taking the caller's
 * hold and handing the replaced node's to the caller; the walk goes on with
 * what follows. False, with nothing changed, when storage runs out. */
bool markdown_core_iter_replace_current(markdown_core_iter *iter, markdown_core_node_pool *pool,
                                        markdown_core_node *node, markdown_core_node **replaced);
/* Takes the sibling just after the current node out of its parent and hands
 * the hold to the caller. False, with nothing changed, when storage runs
 * out. */
bool markdown_core_iter_take_next(markdown_core_iter *iter, markdown_core_node_pool *pool, markdown_core_node **taken);
/* The sibling just after the current node, or NULL. */
static inline markdown_core_node *markdown_core_iter_next_sibling(const markdown_core_iter *iter) {
    const markdown_core_iter_frame *parent = &iter->path->frames[iter->path->count - 2];
    return parent->at + 1 < markdown_core_children_count(parent->node->children)
               ? markdown_core_children_at(parent->node->children, parent->at + 1)
               : NULL;
}

/* A walk with a path of its own, for a caller outside the parse. */
markdown_core_iter *markdown_core_iter_new(markdown_core_node *root);
void markdown_core_iter_free(markdown_core_iter *iter);
markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter);
markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter);

/* Whether consolidation has anything to do at the current Text's EXIT: a
 * Text sibling to absorb, or no bytes of its own to keep. The step below
 * answers the same two questions itself; this is what lets the walk ask them
 * in place and enter the step only when one holds. */
static inline bool markdown_core_text_needs_consolidation(const markdown_core_iter *iter,
                                                          const markdown_core_node *text) {
    size_t count = iter->path->count;
    if (count - iter->base < 2) {
        return false;
    }
    if (text->as.literal->len == 0) {
        return true;
    }
    const markdown_core_iter_frame *parent = &iter->path->frames[count - 2];
    const markdown_core_run *siblings = parent->node->children;
    return parent->at + 1 < siblings->total &&
           markdown_core_children_at(siblings, parent->at + 1)->kind == MARKDOWN_CORE_NODE_TEXT;
}

/* TEXT CONSOLIDATION IS ONE STEP OF A WALK, not a walk of its own.
 *
 * Called with `iter` at `cur`'s EXIT, `cur` a Text that is not the walk's
 * root: absorb every Text sibling that follows `cur` into it (literal, source
 * runs, end position), taking each out of the parent, and drop `cur` itself
 * if it owns no bytes. The merged literal is allocated once, at the run's
 * summed length. Region sets are kept in step when `parser` is given: the
 * survivor takes the runs the nodes it absorbed owned (requirement 11b); the
 * public entry point passes no parser.
 *
 * CONSUMED when `cur` was freed, FAILED on allocation failure, and CONTINUE
 * otherwise. The engine's finish walk runs this before any element step at a
 * Text's EXIT, on the walk's own iterator, which is what keeps an absorbed
 * sibling's events from ever reaching a step.
 *
 * `complete`, when given, is applied to each sibling before it is absorbed,
 * with `depth` as its word-delimiter depth: the walk completes a node at its
 * ENTER, and an absorbed sibling is never entered, so this is where its
 * completion happens. The public entry point passes none. */
typedef void (*markdown_core_complete_node_func)(struct markdown_core_parser *, markdown_core_node *, int);
markdown_core_finish_result markdown_core_consolidate_text_step(struct markdown_core_parser *parser,
                                                                markdown_core_iter *iter, markdown_core_node *cur,
                                                                markdown_core_complete_node_func complete, int depth);

/* The step applied at every Text EXIT of a walk over `root`. */
int markdown_core_consolidate_text_nodes_with_parser(struct markdown_core_parser *parser, markdown_core_node *root);

#ifdef __cplusplus
}
#endif

#endif
