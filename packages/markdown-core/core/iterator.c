#include <assert.h>
#include <stdlib.h>

#include "config.h"
#include "node.h"
#include "markdown-core.h"
#include "parser.h"
#include "iterator.h"

markdown_core_iter *markdown_core_iter_new(markdown_core_node *root) {
    if (root == NULL) {
        return NULL;
    }
    markdown_core_mem *mem = root->content.mem;
    markdown_core_iter *iter = (markdown_core_iter *)mem->calloc(1, sizeof(markdown_core_iter));
    if (!iter) {
        return NULL;
    }
    iter->mem = mem;
    iter->root = root;
    iter->cur.ev_type = MARKDOWN_CORE_EVENT_NONE;
    iter->cur.node = NULL;
    iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
    iter->next.node = root;
    return iter;
}

void markdown_core_iter_free(markdown_core_iter *iter) { iter->mem->free(iter); }

markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter) {
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

void markdown_core_iter_reset(markdown_core_iter *iter, markdown_core_node *current,
                              markdown_core_event_type event_type) {
    iter->next.ev_type = event_type;
    iter->next.node = current;
    markdown_core_iter_next(iter);
}

markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter) { return iter->cur.node; }

markdown_core_event_type markdown_core_iter_get_event_type(markdown_core_iter *iter) { return iter->cur.ev_type; }

markdown_core_node *markdown_core_iter_get_root(markdown_core_iter *iter) { return iter->root; }

int markdown_core_consolidate_text_nodes(markdown_core_node *root) {
    return markdown_core_consolidate_text_nodes_with_parser(NULL, root);
}

/* Decode a contextual space escape only after inline ownership is final.
 * Both spellings are two bytes, and retain the authored two-byte source map.
 * Failed script candidates therefore keep the inherited literal unchanged. */
static void decode_space_escape(markdown_core_node *node, int script_depth) {
    if (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) {
        if (script_depth > 0) {
            markdown_core_chunk_free(node->content.mem, node->as.literal);
            *node->as.literal = markdown_core_chunk_literal("\xC2\xA0");
        }
        node->flags &= ~MARKDOWN_CORE_NODE__ESCAPED_SPACE;
    }
}

/* The surviving Text owns the concatenated literal and a concatenation of
 * its operands' source runs. A caller outside a parse has no parser-owned
 * map to retain and uses the public entry point with NULL. */
int markdown_core_consolidate_text_nodes_with_parser(markdown_core_parser *parser, markdown_core_node *root) {
    if (root == NULL) {
        return 1;
    }
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(root->content.mem);
    markdown_core_event_type ev_type;
    markdown_core_node *cur, *tmp, *next;
    int ok = 1;
    int script_depth = 0;

    if (!iter) {
        return 0;
    }

    /* EXIT, not ENTER, and that is Step 5's mutation rule: the only node a walk
     * may free is the one whose EXIT is current. `TEXT` was in the old
     * `S_is_leaf` list, so its EXIT was suppressed and freeing at ENTER
     * happened to be safe; with the contract total it is a use-after-free. */
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (cur->kind == MARKDOWN_CORE_NODE_SUPERSCRIPT || cur->kind == MARKDOWN_CORE_NODE_SUBSCRIPT) {
            script_depth += ev_type == MARKDOWN_CORE_EVENT_ENTER ? 1 : -1;
        }
        if (ev_type != MARKDOWN_CORE_EVENT_EXIT || cur->kind != MARKDOWN_CORE_NODE_TEXT) {
            continue;
        }
        decode_space_escape(cur, script_depth);

        if (cur->next && cur->next->kind == MARKDOWN_CORE_NODE_TEXT) {
            markdown_core_node combined_map = {0};
            if (parser &&
                !markdown_core_parser_append_content_marks(parser, cur, &combined_map, 0, cur->as.literal->len, 0)) {
                goto failed;
            }
            markdown_core_strbuf_clear(&buf);
            markdown_core_strbuf_put(&buf, cur->as.literal->data, cur->as.literal->len);
            if (buf.oom) {
                goto failed;
            }
            tmp = cur->next;
            while (tmp && tmp->kind == MARKDOWN_CORE_NODE_TEXT) {
                /* Bring `tmp` to its own EXIT before freeing it: two events
                 * now, where a suppressed EXIT used to make one enough. */
                markdown_core_iter_next(iter); /* tmp ENTER */
                markdown_core_iter_next(iter); /* tmp EXIT  */
                decode_space_escape(tmp, script_depth);
                if (parser && !markdown_core_parser_append_content_marks(parser, tmp, &combined_map, 0,
                                                                         tmp->as.literal->len, buf.size)) {
                    goto failed;
                }
                markdown_core_strbuf_put(&buf, tmp->as.literal->data, tmp->as.literal->len);
                if (buf.oom) {
                    goto failed;
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
                markdown_core_node_free(tmp);
                tmp = next;
            }
            /* Every node the loop freed was ahead of the cursor and is now
             * unlinked, so the cursor sits at the last one's EXIT. Re-establish
             * `cur`'s EXIT: it recomputes the lookahead from the siblings that
             * survived, and it is what makes the drop below legal under the
             * rule rather than merely safe. */
            if (parser) {
                cur->content_mark = combined_map.content_mark;
                cur->content_mark_count = combined_map.content_mark_count;
                cur->content_mark_offset = 0;
            }
            markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_EXIT);
            markdown_core_chunk_free(iter->mem, cur->as.literal);
            *cur->as.literal = markdown_core_chunk_buf_detach(&buf);
            if (!cur->as.literal->data) {
                // The buffer was poisoned, so this run's bytes are LOST rather
                // than absent. Report it and leave the node where it is: the
                // drop below must only ever remove a node that is honestly
                // empty, never one an allocation failure emptied.
                goto failed;
            }
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
        // one's subtree.
        if (cur->as.literal->len == 0) {
            markdown_core_chunk_free(iter->mem, cur->as.literal);
            markdown_core_node_free(cur);
        }
    }

    goto done;
failed:
    ok = 0;
done:
    markdown_core_strbuf_free(&buf);
    markdown_core_iter_free(iter);
    return ok;
}
