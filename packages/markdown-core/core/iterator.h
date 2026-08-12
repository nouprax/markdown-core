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
};

/** Re-seats the walk on `current` with `event_type` pending, discarding the
 * successor the iterator had already computed.
 *
 * `markdown_core_iter_next` decides the successor BEFORE it hands the event
 * to the caller, so a consumer that unlinks or frees nodes inside the loop
 * body invalidates a decision that was already made. Re-seating is how such a
 * consumer states where the walk resumes; the alternative -- calling
 * `markdown_core_iter_next` once per node it is about to free -- makes the
 * consumer depend on how many events the iterator emits per node, which is
 * not part of any contract it can rely on. */
void markdown_core_iter_reset(
    markdown_core_iter *iter,
    markdown_core_node *current,
    markdown_core_event_type event_type
);

#ifdef __cplusplus
}
#endif

#endif
