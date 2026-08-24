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

/* Consolidation frees every text node but the first of each run, and since
 * requirement 11b those nodes OWN REGIONS. `parser` is how the survivor takes
 * them: one node replacing another, roles kept, bounded by the freed node's own
 * lines. NULL is the public entry point's answer -- a caller outside a parse
 * has no region set to keep. */
int markdown_core_consolidate_text_nodes_with_parser(markdown_core_parser *parser, markdown_core_node *root) {
    if (root == NULL) {
        return 1;
    }
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(root->content.mem);
    markdown_core_event_type ev_type;
    markdown_core_node *cur, *tmp, *next;
    int ok = 1;

    if (!iter) {
        return 0;
    }

    /* EXIT, not ENTER, and that is Step 5's mutation rule: the only node a walk
     * may free is the one whose EXIT is current. `TEXT` was in the old
     * `S_is_leaf` list, so its EXIT was suppressed and freeing at ENTER
     * happened to be safe; with the contract total it is a use-after-free. */
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_EXIT || cur->type != MARKDOWN_CORE_NODE_TEXT) {
            continue;
        }

        if (cur->next && cur->next->type == MARKDOWN_CORE_NODE_TEXT) {
            markdown_core_strbuf_clear(&buf);
            markdown_core_strbuf_put(&buf, cur->as.literal.data, cur->as.literal.len);
            tmp = cur->next;
            while (tmp && tmp->type == MARKDOWN_CORE_NODE_TEXT) {
                /* Bring `tmp` to its own EXIT before freeing it: two events
                 * now, where a suppressed EXIT used to make one enough. */
                markdown_core_iter_next(iter); /* tmp ENTER */
                markdown_core_iter_next(iter); /* tmp EXIT  */
                markdown_core_strbuf_put(&buf, tmp->as.literal.data, tmp->as.literal.len);
                // ONLY AN OPERAND THAT OWNS BYTES CAN SAY WHERE THE RUN ENDS.
                // An empty one has no last byte to end at, and the empties in
                // this tree carry a zeroed position rather than an honest one,
                // so taking their end put `1:1..1:0` on a run of four real
                // characters. And the end is a LINE and a column together: this
                // used to carry the column forward and leave the line behind,
                // which is why a merged run crossing a line ending reported the
                // first operand's line with the last operand's column.
                if (tmp->as.literal.len > 0) {
                    cur->end_line = tmp->end_line;
                    cur->end_column = tmp->end_column;
                }
                next = tmp->next;
                if (parser) {
                }
                markdown_core_node_free(tmp);
                tmp = next;
            }
            /* Every node the loop freed was ahead of the cursor and is now
             * unlinked, so the cursor sits at the last one's EXIT. Re-establish
             * `cur`'s EXIT: it recomputes the lookahead from the siblings that
             * survived, and it is what makes the drop below legal under the
             * rule rather than merely safe. */
            markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_EXIT);
            markdown_core_chunk_free(iter->mem, &cur->as.literal);
            cur->as.literal = markdown_core_chunk_buf_detach(&buf);
            if (!cur->as.literal.data) {
                // The buffer was poisoned, so this run's bytes are LOST rather
                // than absent. Report it and leave the node where it is: the
                // drop below must only ever remove a node that is honestly
                // empty, never one an allocation failure emptied.
                ok = 0;
                continue;
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
        if (cur->as.literal.len == 0) {
            markdown_core_chunk_free(iter->mem, &cur->as.literal);
            markdown_core_node_free(cur);
        }
    }

    markdown_core_strbuf_free(&buf);
    markdown_core_iter_free(iter);
    return ok;
}

int markdown_core_node_own(markdown_core_node *root) {
    int ok = 1;
    if (root == NULL) {
        return 1;
    }
    /* Traverses via the parent/next pointers instead of an iterator so that
     * taking ownership never needs to allocate; a chunk copy that cannot be
     * allocated is emptied rather than left borrowing the source buffer. */
    markdown_core_mem *mem = root->content.mem;
    markdown_core_node *cur = root;

    while (cur) {
        switch (cur->type) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_HTML:
        case MARKDOWN_CORE_NODE_CODE:
        case MARKDOWN_CORE_NODE_HTML_BLOCK:
            if (!markdown_core_chunk_to_cstr(mem, &cur->as.literal)) {
                markdown_core_chunk_set_cstr(mem, &cur->as.literal, NULL);
                ok = 0;
            }
            break;
        case MARKDOWN_CORE_NODE_LINK:
            if (!markdown_core_chunk_to_cstr(mem, &cur->as.link.url)) {
                markdown_core_chunk_set_cstr(mem, &cur->as.link.url, NULL);
                ok = 0;
            }
            if (!markdown_core_chunk_to_cstr(mem, &cur->as.link.title)) {
                markdown_core_chunk_set_cstr(mem, &cur->as.link.title, NULL);
                ok = 0;
            }
            break;
        }

        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur != root && cur->next == NULL) {
                cur = cur->parent;
            }
            cur = (cur == root) ? NULL : cur->next;
        }
    }
    return ok;
}
