#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_node *node;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_mem *mem;
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
    /* The borrower whose aliased list the walk is inside, or NULL. A shared
     * child has no parent (node.h), so the climb out of the list lands here
     * -- in the borrower THIS walk entered through, whichever other trees
     * alias the same list. One slot, because a list is one level deep. A
     * reset into a shared list is well defined only from a walk that entered
     * its borrower. */
    markdown_core_node *borrower;
};

/* THE SAME WALK WITHOUT THE ALLOCATION (docs/STREAMING.md T20).
 * `markdown_core_iter_new` callocs, which is right for a walk taken once per
 * document and wrong once per block per pass per feed: three passes over a
 * hundred-block document go from 3 allocations to 300. The struct is complete
 * here, so a caller inside the engine holds it on its stack. `root` must not
 * be NULL; there is no failure to report. */
void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_node *root);

#ifdef __cplusplus
}
#endif

#endif
