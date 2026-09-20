#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"
#include "markdown-core-element-api.h"
#include "buffer.h"

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_node *node;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
};

/* TEXT CONSOLIDATION IS ONE STEP OF A WALK, not a walk of its own.
 *
 * Called with `iter` at `cur`'s EXIT, `cur` a Text: absorb every Text sibling
 * that follows `cur` into it (literal, source runs, end position), advancing
 * `iter` over each absorbed sibling's ENTER and EXIT and then re-establishing
 * `cur`'s EXIT so the lookahead names a survivor; and drop `cur` itself if it
 * owns no bytes. `scratch` is the caller's buffer for the concatenation, so a
 * walk allocates it once and not once per run. Region sets are kept in step
 * when `parser` is given: the survivor takes the runs the nodes it absorbed
 * owned (requirement 11b); the public entry point passes no parser.
 *
 * CONSUMED when `cur` was freed, FAILED on allocation failure (the tree is
 * consistent: every absorbed operand was unlinked before it was freed), and
 * CONTINUE otherwise. The engine's finish walk runs this before any element
 * step at a Text's EXIT, on the walk's own iterator, which is what keeps an
 * absorbed sibling's events from ever reaching a step. */
markdown_core_finish_result markdown_core_consolidate_text_step(struct markdown_core_parser *parser,
                                                                markdown_core_iter *iter, markdown_core_node *cur,
                                                                markdown_core_strbuf *scratch);

/* The step applied at every Text EXIT of a walk over `root`.
 * `markdown_core_consolidate_text_nodes` is this with no parser. */
int markdown_core_consolidate_text_nodes_with_parser(struct markdown_core_parser *parser, markdown_core_node *root);

#ifdef __cplusplus
}
#endif

#endif
