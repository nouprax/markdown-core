#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "node.h"
#include "markdown-core.h"
#include "parser.h"
#include "iterator.h"

void markdown_core_iter_init(markdown_core_iter *iter, markdown_core_node *root) {
    iter->mem = root->content.mem;
    iter->root = root;
    iter->cur.ev_type = MARKDOWN_CORE_EVENT_NONE;
    iter->cur.node = NULL;
    iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
    iter->next.node = root;
    iter->oom = false;
    iter->frames = iter->inline_frames;
    iter->depth = 0;
    iter->cap = MARKDOWN_CORE_ITER_INLINE_DEPTH;
}

void markdown_core_iter_deinit(markdown_core_iter *iter) {
    if (iter->frames != iter->inline_frames) {
        iter->mem->free(iter->frames);
        iter->frames = iter->inline_frames;
        iter->cap = MARKDOWN_CORE_ITER_INLINE_DEPTH;
    }
    iter->depth = 0;
}

markdown_core_iter *markdown_core_iter_new(markdown_core_node *root) {
    if (root == NULL) {
        return NULL;
    }
    markdown_core_mem *mem = root->content.mem;
    markdown_core_iter *iter = (markdown_core_iter *)mem->calloc(1, sizeof(markdown_core_iter));
    if (!iter) {
        return NULL;
    }
    markdown_core_iter_init(iter, root);
    return iter;
}

void markdown_core_iter_free(markdown_core_iter *iter) {
    markdown_core_iter_deinit(iter);
    iter->mem->free(iter);
}

/* Push `node` as the ancestor the walk is entering. A failed spill raises
 * `oom` and refuses the push; the caller then skips the subtree, so the
 * walk stays well formed and merely incomplete. */
static bool S_iter_push(markdown_core_iter *iter, markdown_core_node *node) {
    if (iter->depth == iter->cap) {
        size_t grown = iter->cap * 2;
        markdown_core_iter_frame *frames;
        if (iter->frames == iter->inline_frames) {
            frames = (markdown_core_iter_frame *)iter->mem->calloc(grown, sizeof(*frames));
            if (frames) {
                memcpy(frames, iter->frames, iter->depth * sizeof(*frames));
            }
        } else {
            frames = (markdown_core_iter_frame *)iter->mem->realloc(iter->frames, grown * sizeof(*frames));
        }
        if (!frames) {
            iter->oom = true;
            return false;
        }
        iter->frames = frames;
        iter->cap = grown;
    }
    iter->frames[iter->depth].node = node;
    iter->frames[iter->depth].index = 0;
    iter->depth++;
    return true;
}

markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter) {
    markdown_core_event_type ev_type = iter->next.ev_type;
    markdown_core_node *node = iter->next.node;

    iter->cur.ev_type = ev_type;
    iter->cur.node = node;

    if (ev_type == MARKDOWN_CORE_EVENT_DONE) {
        return ev_type;
    }

    if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
        markdown_core_child_cursor cursor;
        markdown_core_node *first = markdown_core_child_first(node, &cursor);
        if (first && S_iter_push(iter, node)) {
            iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
            iter->next.node = first;
        } else {
            /* Childless, or a spill the allocator refused: this node closes
             * next either way. */
            iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
        }
    } else if (iter->depth == 0) {
        /* The root's EXIT: the walk is over and the spill goes back. */
        markdown_core_iter_deinit(iter);
        iter->next.ev_type = MARKDOWN_CORE_EVENT_DONE;
        iter->next.node = NULL;
    } else {
        markdown_core_iter_frame *top = &iter->frames[iter->depth - 1];
        markdown_core_node *sibling;
        if (MARKDOWN_CORE_NODE_ARRAY_P(top->node)) {
            top->index++;
            sibling = top->index < top->node->children.count ? top->node->children.vec[top->index] : NULL;
        } else {
            sibling = node->next;
        }
        if (sibling) {
            iter->next.ev_type = MARKDOWN_CORE_EVENT_ENTER;
            iter->next.node = sibling;
        } else {
            iter->depth--;
            iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
            iter->next.node = top->node;
        }
    }

    return ev_type;
}

/* Re-deliver `current`'s EXIT so the walk recomputes its lookahead from the
 * siblings that now stand there. Valid ONLY for a node under an INTRUSIVE,
 * OWNED parent whose list was edited AHEAD of the cursor -- consolidation's
 * idiom -- because an intrusive level keeps no cursor state to fall out of
 * step with; an array level does, and its lists are never edited mid-walk.
 * The lookahead being re-wound may already have POPPED the parent's frame
 * (it had computed the parent's own EXIT), so the parent is re-armed here;
 * it is readable off the node exactly because the list is owned. */
void markdown_core_iter_reset(
    markdown_core_iter *iter,
    markdown_core_node *current,
    markdown_core_event_type event_type
) {
    if (event_type == MARKDOWN_CORE_EVENT_EXIT && current != iter->root && current->parent &&
        (iter->depth == 0 || iter->frames[iter->depth - 1].node != current->parent)) {
        S_iter_push(iter, current->parent);
    }
    iter->next.ev_type = event_type;
    iter->next.node = current;
    markdown_core_iter_next(iter);
}

void markdown_core_iter_skip_children(markdown_core_iter *iter) {
    markdown_core_node *current = iter->cur.node;
    assert(iter->cur.ev_type == MARKDOWN_CORE_EVENT_ENTER);
    if (iter->next.ev_type == MARKDOWN_CORE_EVENT_ENTER && iter->depth > 0 &&
        iter->frames[iter->depth - 1].node == current) {
        iter->depth--;
    }
    iter->next.ev_type = MARKDOWN_CORE_EVENT_EXIT;
    iter->next.node = current;
}

markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter) { return iter->cur.node; }

int markdown_core_consolidate_text_nodes(markdown_core_node *root) {
    if (root == NULL) {
        return 1;
    }
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(root->content.mem);
    markdown_core_event_type ev_type;
    markdown_core_node *cur, *tmp, *next;
    int ok = 1;

    markdown_core_iter_init(iter, root);

    /* EXIT, not ENTER, and that is Step 5's mutation rule: the only node a walk
     * may free is the one whose EXIT is current. `TEXT` was in the old
     * `S_is_leaf` list, so its EXIT was suppressed and freeing at ENTER
     * happened to be safe; with the contract total it is a use-after-free. */
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        /* A SHARED text node is a retained projection's (review-found):
         * its literal and its links are every tree's at once, so it
         * neither merges nor drops here. Its runs were consolidated
         * before the store, and a shared node is never the intrusive
         * sibling of a fresh one, so skipping it skips whole frozen
         * runs, not halves of mixed ones. */
        if (ev_type != MARKDOWN_CORE_EVENT_EXIT || cur->type != MARKDOWN_CORE_NODE_TEXT ||
            (cur->flags & MARKDOWN_CORE_NODE__SHARED)) {
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
    if (iter->oom) {
        /* A refused spill truncated the walk (iterator.h): runs past the
         * truncation were never consolidated, and the caller treats this
         * exactly as it treats a lost merge buffer. */
        ok = 0;
    }
    return ok;
}
