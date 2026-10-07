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

/* A depth-first walk over a subtree being built (node.h,
 * markdown_core_member): every member yields exactly one ENTER and one EXIT,
 * with its descendants' events between them, and DONE follows the root's
 * EXIT. Field roots are independent roots and are never discovered by this
 * traversal. While walking, the only member that may be released is the one
 * whose EXIT is current: that is the one moment the lookahead names something
 * outside its own subtree. `markdown_core_iter_reset(iter, member,
 * MARKDOWN_CORE_EVENT_EXIT)` brings a member back under that rule after
 * mutating around it. */
typedef struct markdown_core_iter markdown_core_iter;

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_member *member;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_member *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
};

/* THE ITERATOR'S STEP, IN THE HEADER. An inline root's completion takes one
 * per event of every member of its tree, and it keeps its iterators in its
 * own frames rather than behind an allocation, so the step is here, where it
 * can keep the state in registers instead of calling across a translation
 * unit for it. */
static inline void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_member *root) {
    iter->root = root;
    iter->cur.ev_type = MARKDOWN_CORE_EVENT_NONE;
    iter->cur.member = NULL;
    iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
    iter->next.member = root;
}

static inline markdown_core_event_type markdown_core_iter_step(markdown_core_iter *iter) {
    markdown_core_event_type ev_type = iter->next.ev_type;
    markdown_core_member *member = iter->next.member;

    iter->cur.ev_type = ev_type;
    iter->cur.member = member;

    if (ev_type == MARKDOWN_CORE_EVENT_DONE) {
        return ev_type;
    }

    /* roll forward to next item, setting both fields */
    if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
        if (member->first == NULL) {
            /* stay on this member but exit */
            iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
        } else {
            iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
            iter->next.member = member->first;
        }
    } else if (member == iter->root) {
        /* don't move past root */
        iter->next.ev_type = MARKDOWN_CORE_EVENT_DONE;
        iter->next.member = NULL;
    } else if (member->next) {
        iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
        iter->next.member = member->next;
    } else {
        assert(member->owner);
        iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
        iter->next.member = member->owner;
    }

    return ev_type;
}

/* The new current member must be `root` or one of its descendants. */
void markdown_core_iter_reset(markdown_core_iter *iter, markdown_core_member *current,
                              markdown_core_event_type event_type);

/* Whether consolidation has anything to do at `text`'s EXIT: a Text sibling
 * to absorb, or no bytes of its own to keep. The step below answers the same
 * two questions itself; this is what lets the walk ask them in place and
 * enter the step only when one holds. */
static inline bool markdown_core_text_needs_consolidation(const markdown_core_member *text) {
    return (text->next && text->next->node->kind == MARKDOWN_CORE_NODE_TEXT) || text->node->as.literal->len == 0;
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
 * CONTINUE otherwise. An inline root's completion runs this before any
 * element step at a Text's EXIT, on the pass's own iterator, which is what
 * keeps an absorbed sibling's events from ever reaching a step.
 *
 * `complete`, when given, is applied to each sibling before it is absorbed,
 * with `depth` as its word-delimiter depth: the pass completes a node at its
 * ENTER, and an absorbed sibling's ENTER is stepped over here, so this is
 * where its completion happens. The public entry point passes none. */
typedef void (*markdown_core_complete_node_func)(struct markdown_core_parser *, markdown_core_node *, int);
markdown_core_complete_result markdown_core_consolidate_text_step(struct markdown_core_parser *parser,
                                                                  markdown_core_iter *iter, markdown_core_member *cur,
                                                                  markdown_core_complete_node_func complete, int depth);

#ifdef __cplusplus
}
#endif

#endif
