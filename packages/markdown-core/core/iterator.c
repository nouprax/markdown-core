#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "markdown-core.h"
#include "parser.h"
#include "iterator.h"

markdown_core_iter *markdown_core_iter_new(markdown_core_node *root) {
    if (root == NULL) {
        return NULL;
    }
    markdown_core_iter *iter = (markdown_core_iter *)markdown_core_alloc(1, sizeof(markdown_core_iter));
    if (!iter) {
        return NULL;
    }
    markdown_core_iter_init(iter, root);
    return iter;
}

void markdown_core_iter_free(markdown_core_iter *iter) { markdown_core_free(iter); }

markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter) { return markdown_core_iter_step(iter); }

void markdown_core_iter_reset(markdown_core_iter *iter, markdown_core_node *current,
                              markdown_core_event_type event_type) {
    iter->next.ev_type = event_type;
    iter->next.node = current;
    markdown_core_iter_step(iter);
}

markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter) { return iter->cur.node; }

markdown_core_event_type markdown_core_iter_get_event_type(markdown_core_iter *iter) { return iter->cur.ev_type; }

markdown_core_node *markdown_core_iter_get_root(markdown_core_iter *iter) { return iter->root; }

int markdown_core_consolidate_text_nodes(markdown_core_node *root) {
    return markdown_core_consolidate_text_nodes_with_parser(NULL, root);
}

/* Every iterator step a finish-stage consolidation takes is counted on the
 * parser when there is one, so the traversal count the finish stage claims
 * can be checked (see the counters in parser.h). */
static void S_count_step(markdown_core_parser *parser, markdown_core_event_type event) {
    if (!parser) {
        return;
    }
    parser->finish_walk_events++;
    if (event == MARKDOWN_CORE_EVENT_ENTER) {
        parser->finish_nodes_entered++;
    } else if (event == MARKDOWN_CORE_EVENT_DONE) {
        parser->finish_walk_roots++;
    }
}

/* The surviving Text owns the concatenated literal and a concatenation of
 * its operands' source runs. A caller outside a parse has no parser-owned
 * map to retain and uses the public entry point with NULL.
 *
 * EXIT, not ENTER, and that is Step 5's mutation rule: the only node a walk
 * may free is the one whose EXIT is current. `TEXT` was in the old
 * `S_is_leaf` list, so its EXIT was suppressed and freeing at ENTER
 * happened to be safe; with the contract total it is a use-after-free. */
markdown_core_finish_result markdown_core_consolidate_text_step(markdown_core_parser *parser, markdown_core_iter *iter,
                                                                markdown_core_node *cur,
                                                                markdown_core_complete_node_func complete, int depth) {
    markdown_core_node *tmp, *next;

    assert(iter->cur.node == cur && iter->cur.ev_type == MARKDOWN_CORE_EVENT_EXIT);
    assert(cur->kind == MARKDOWN_CORE_NODE_TEXT);

    if (cur->next && cur->next->kind == MARKDOWN_CORE_NODE_TEXT) {
        /* THE MERGED TEXT'S MAP IS A VIEW WHEN ITS OPERANDS ARE. A Text that
         * is a verbatim copy of its source holds a slice of its container's
         * runs (markdown_core_inline_map_text), and the siblings absorbed
         * here were placed left to right in that container, so while each
         * operand's bytes begin where the previous one's ended and its first
         * run is the previous one's last or the one after, the union is the
         * slice from the first operand's first run to the last operand's
         * last, at the first operand's offset -- the same runs the copy below
         * would append, answering every position the same way, and nothing
         * is appended. A decoded operand (its own run, at offset zero, a
         * literal shorter than its scope) or an operand with no map breaks
         * the chain, and the union is materialized run by run as before. */
        markdown_core_content_map combined_map = {0};
        bool view = parser && cur->content_map.count > 0;
        int view_end = cur->content_map.first + cur->content_map.count;
        bufsize_t view_offset = cur->content_map.offset + cur->as.literal->len;
        /* THE MERGED LITERAL IS ALLOCATED ONCE, at its length. Every operand
         * is known before the first is absorbed, so the run's length is a sum
         * taken here, and each operand is copied to its place: no buffer
         * grows, and none is handed over and grown again for the next run. */
        size_t length = (size_t)cur->as.literal->len;
        for (tmp = cur->next; tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT; tmp = tmp->next) {
            length += (size_t)tmp->as.literal->len;
            if (!view) {
                continue;
            }
            view = tmp->content_map.count > 0 && tmp->content_map.offset == view_offset &&
                   tmp->content_map.first >= view_end - 1 && tmp->content_map.first <= view_end;
            view_end = tmp->content_map.first + tmp->content_map.count;
            view_offset += tmp->as.literal->len;
        }
        /* The bound every literal buffer shares. */
        if (length > (size_t)MARKDOWN_CORE_STRBUF_LIMIT) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        if (view) {
            combined_map.first = cur->content_map.first;
            combined_map.count = view_end - cur->content_map.first;
            combined_map.offset = cur->content_map.offset;
        } else if (parser && !markdown_core_parser_append_content_marks(parser, &cur->content_map, &combined_map, 0,
                                                                        cur->as.literal->len, 0)) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        unsigned char *merged = markdown_core_realloc(NULL, length + 1);
        if (!merged) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        bufsize_t at = cur->as.literal->len;
        if (at) {
            memcpy(merged, cur->as.literal->data, (size_t)at);
        }
        tmp = cur->next;
        while (tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT) {
            /* Bring `tmp` to its own EXIT before freeing it: two events now,
             * where a suppressed EXIT used to make one enough. They are steps
             * of the walk this is part of, taken HERE so that no step after
             * this one is ever handed a node this one is about to free. */
            S_count_step(parser, markdown_core_iter_next(iter)); /* tmp ENTER */
            S_count_step(parser, markdown_core_iter_next(iter)); /* tmp EXIT  */
            if (complete) {
                complete(parser, tmp, depth);
            }
            if (parser && !view &&
                !markdown_core_parser_append_content_marks(parser, &tmp->content_map, &combined_map, 0,
                                                           tmp->as.literal->len, at)) {
                markdown_core_free(merged);
                return MARKDOWN_CORE_FINISH_FAILED;
            }
            if (tmp->as.literal->len) {
                memcpy(merged + at, tmp->as.literal->data, (size_t)tmp->as.literal->len);
                at += tmp->as.literal->len;
            }
            // ONLY AN OPERAND THAT OWNS BYTES CAN SAY WHERE THE RUN ENDS.
            // An empty one has no last byte to end at, and the empties in
            // this tree carry a zeroed position rather than an honest one,
            // so taking their end put `1:1..1:0` on a run of four real
            // characters. And the end is a LINE and a column together: this
            // used to carry the column forward and leave the line behind,
            // which is why a merged run crossing a line ending reported the
            // first operand's line with the last operand's column.
            if (tmp->as.literal->len > 0) {
                cur->end_line = tmp->end_line;
                cur->end_column = tmp->end_column;
            }
            next = tmp->next;
            markdown_core_parser_release_node(parser, tmp);
            tmp = next;
        }
        /* Every node the loop freed was ahead of the cursor and is now
         * unlinked, so the cursor sits at the last one's EXIT. Re-establish
         * `cur`'s EXIT: it recomputes the lookahead from the siblings that
         * survived, and it is what makes the drop below legal under the
         * rule rather than merely safe. It is not a step of the walk -- the
         * event it re-delivers was delivered already -- so it is not counted. */
        if (parser) {
            cur->content_map = combined_map;
        }
        markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_EXIT);
        markdown_core_chunk_free(cur->as.literal);
        merged[at] = '\0';
        *cur->as.literal = (markdown_core_chunk){merged, at, 1};
    }

    // A `TEXT` NODE THAT OWNS NO BYTES IS NOT A NODE. It has no literal to
    // render and no source to point at, so the only position it can carry
    // is borrowed or zeroed -- and a consumer that walks children sees a
    // child that is not there. Dropping it here also makes the third
    // producer unreachable by construction: a run of empties can no longer
    // merge into an empty, because the operands are gone before the merge.
    //
    // Freeing here is legal because `cur`'s EXIT is current -- Step 5's
    // mutation rule -- so `iter->next` already names a node outside this
    // one's subtree. The caller learns that `cur` is gone and hands this
    // event to nothing else.
    if (cur->as.literal->len == 0) {
        markdown_core_chunk_free(cur->as.literal);
        markdown_core_parser_release_node(parser, cur);
        return MARKDOWN_CORE_FINISH_CONSUMED;
    }
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

/* The same step, driven by a walk of its own. Inside a parse the finish walk
 * runs the step itself and never comes here; this is the entry point for a
 * tree built or rewritten outside a parse, and a pass that calls it with a
 * parser pays -- and is counted for -- one more traversal of the root. */
int markdown_core_consolidate_text_nodes_with_parser(markdown_core_parser *parser, markdown_core_node *root) {
    if (root == NULL) {
        return 1;
    }
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    int ok = 1;

    if (!iter) {
        return 0;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *cur = markdown_core_iter_get_node(iter);
        S_count_step(parser, ev_type);
        if (ev_type != MARKDOWN_CORE_EVENT_EXIT || cur->kind != MARKDOWN_CORE_NODE_TEXT) {
            continue;
        }
        if (markdown_core_consolidate_text_step(parser, iter, cur, NULL, 0) == MARKDOWN_CORE_FINISH_FAILED) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        S_count_step(parser, MARKDOWN_CORE_EVENT_DONE);
    }

    markdown_core_iter_free(iter);
    return ok;
}
