#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stdbool.h>

#include "markdown-core.h"
#include "markdown-core-element-api.h"
#include "buffer.h"
#include "node.h"

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_node *node;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
};

/* THE ITERATOR'S STEP, IN THE HEADER. The finish walk takes one per event of
 * every node of every root, and it keeps its iterators in its own frames
 * rather than behind an allocation, so the step is here, where the walk can
 * keep the state in registers instead of calling across a translation unit
 * for it: `markdown_core_iter_next` is this behind the public call, and
 * `markdown_core_iter_new` is `markdown_core_iter_init` on a heap iterator. */
static inline void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_node *root) {
    iter->root = root;
    iter->cur.ev_type = MARKDOWN_CORE_EVENT_NONE;
    iter->cur.node = NULL;
    iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
    iter->next.node = root;
}

static inline markdown_core_event_type markdown_core_iter_step(markdown_core_iter *iter) {
    markdown_core_event_type ev_type = iter->next.ev_type;
    markdown_core_node *node = iter->next.node;

    iter->cur.ev_type = ev_type;
    iter->cur.node = node;

    if (ev_type == MARKDOWN_CORE_EVENT_DONE) {
        return ev_type;
    }

    /* roll forward to next item, setting both fields */
    if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
        if (node->first_child == NULL) {
            /* stay on this node but exit */
            iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
        } else {
            iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
            iter->next.node = node->first_child;
        }
    } else if (node == iter->root) {
        /* don't move past root */
        iter->next.ev_type = MARKDOWN_CORE_EVENT_DONE;
        iter->next.node = NULL;
    } else if (node->next) {
        iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
        iter->next.node = node->next;
    } else if (node->parent) {
        iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
        iter->next.node = node->parent;
    } else {
        assert(false);
        iter->next.ev_type = MARKDOWN_CORE_EVENT_DONE;
        iter->next.node = NULL;
    }

    return ev_type;
}

/* Whether consolidation has anything to do at `text`'s EXIT: a Text sibling
 * to absorb, or no bytes of its own to keep. The step below answers the same
 * two questions itself; this is what lets the walk ask them in place and
 * enter the step only when one holds. */
static inline bool markdown_core_text_needs_consolidation(const markdown_core_node *text) {
    return (text->next && text->next->kind == MARKDOWN_CORE_NODE_TEXT) || text->as.literal->len == 0;
}

/* TEXT CONSOLIDATION IS ONE STEP OF A WALK, not a walk of its own.
 *
 * Called with `iter` at `cur`'s EXIT, `cur` a Text: absorb every Text sibling
 * that follows `cur` into it (literal, source runs, end position), advancing
 * `iter` over each absorbed sibling's ENTER and EXIT and then re-establishing
 * `cur`'s EXIT so the lookahead names a survivor; and drop `cur` itself if it
 * owns no bytes. The merged literal is allocated once, at the run's summed
 * length. Region sets are kept in step when `parser` is given: the survivor
 * takes the runs the nodes it absorbed owned (requirement 11b); the public
 * entry point passes no parser.
 *
 * CONSUMED when `cur` was freed, FAILED on allocation failure (the tree is
 * consistent: every absorbed operand was unlinked before it was freed), and
 * CONTINUE otherwise. The engine's finish walk runs this before any element
 * step at a Text's EXIT, on the walk's own iterator, which is what keeps an
 * absorbed sibling's events from ever reaching a step.
 *
 * `complete`, when given, is applied to each sibling before it is absorbed,
 * with `depth` as its word-delimiter depth: the walk completes a node at its
 * ENTER, and an absorbed sibling's ENTER is stepped over here, so this is
 * where its completion happens. The public entry point passes none. */
typedef void (*markdown_core_complete_node_func)(struct markdown_core_parser *, markdown_core_node *, int);
markdown_core_finish_result markdown_core_consolidate_text_step(struct markdown_core_parser *parser,
                                                                markdown_core_iter *iter, markdown_core_node *cur,
                                                                markdown_core_complete_node_func complete, int depth);

/* The step applied at every Text EXIT of a walk over `root`.
 * `markdown_core_consolidate_text_nodes` is this with no parser. */
int markdown_core_consolidate_text_nodes_with_parser(struct markdown_core_parser *parser, markdown_core_node *root);

#ifdef __cplusplus
}
#endif

#endif
