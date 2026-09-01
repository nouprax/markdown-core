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

/* One ancestor the walk is inside. `index` is the vector cursor for a
 * CHILD_ARRAY parent and meaningless for an intrusive one, whose sibling
 * step is the current child's own `next` (node.h, the child cursor). */
typedef struct {
    markdown_core_node *node;
    size_t index;
} markdown_core_iter_frame;

/* THE WALK CARRIES ITS OWN ANCESTRY (#161, D9). The old iterator climbed
 * `parent` pointers and kept ONE borrower slot for the one parentless level
 * a tree could hold. A derived tree's containers are vectors now, their
 * sibling order is the parent's fact, and a shared child carries no parent
 * at all -- so the frames above are the ancestry, and neither parent
 * pointers nor borrower slots take any part in the walk. The first
 * MARKDOWN_CORE_ITER_INLINE_DEPTH levels live inside the struct (T20: a
 * walk taken per block per pass must not allocate); deeper trees spill to
 * the heap, the spill is released when the walk reaches DONE, and
 * `markdown_core_iter_deinit` releases it for a walk abandoned early. A
 * spill that cannot allocate truncates the walk -- the subtree is skipped,
 * `oom` is raised, and the caller that cannot tolerate a partial walk
 * checks it. */
#define MARKDOWN_CORE_ITER_INLINE_DEPTH 24

struct markdown_core_iter {
    markdown_core_mem *mem;
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
    bool oom;
    markdown_core_iter_frame *frames;
    size_t depth;
    size_t cap;
    markdown_core_iter_frame inline_frames[MARKDOWN_CORE_ITER_INLINE_DEPTH];
};

/* THE SAME WALK WITHOUT THE ALLOCATION (docs/STREAMING.md T20).
 * `markdown_core_iter_new` callocs, which is right for a walk taken once per
 * document and wrong once per block per pass per feed: three passes over a
 * hundred-block document go from 3 allocations to 300. The struct is complete
 * here, so a caller inside the engine holds it on its stack. `root` must not
 * be NULL; there is no failure to report. */
void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_node *root);

/* Release a spilled frame stack. Needed only by a caller that abandons a
 * walk before DONE; a walk that completes released it already, and calling
 * this afterwards is a harmless no-op. */
void markdown_core_iter_deinit(markdown_core_iter *iter);

/* Valid only while `cur` is the ENTER of a node: the walk proceeds directly
 * to that node's EXIT, entering none of its children. This is F22's rule as
 * a primitive -- a borrowed or shared subtree is skipped, never entered to
 * write -- and it replaces the reset-to-EXIT idiom. */
void markdown_core_iter_skip_children(markdown_core_iter *iter);

#ifdef __cplusplus
}
#endif

#endif
