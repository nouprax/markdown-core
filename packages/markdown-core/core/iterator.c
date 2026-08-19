#include <assert.h>
#include <stdlib.h>

#include "config.h"
#include "node.h"
#include "markdown-core.h"
#include "iterator.h"

markdown_core_iter *markdown_core_iter_new(markdown_core_node *root) {
    if (root == NULL) {
        return NULL;
    }
    markdown_core_mem *mem = root->content.mem;
    markdown_core_iter *iter = (markdown_core_iter *)mem->calloc(mem, 1, sizeof(markdown_core_iter));
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

void markdown_core_iter_free(markdown_core_iter *iter) { iter->mem->free(iter->mem, iter); }

void markdown_core_iter_reset(
    markdown_core_iter *iter,
    markdown_core_node *current,
    markdown_core_event_type event_type
) {
    iter->cur.ev_type = event_type;
    iter->cur.node = current;
    iter->next.ev_type = event_type;
    iter->next.node = current;
}

markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter) {
    markdown_core_event_type ev_type = iter->next.ev_type;
    markdown_core_node *node = iter->next.node;

    iter->cur.ev_type = ev_type;
    iter->cur.node = node;

    if (ev_type == MARKDOWN_CORE_EVENT_DONE) {
        return ev_type;
    }

    /* roll forward to next item, setting both fields */
    /* EVERY node is entered and exited. Upstream skipped the exit for eight
     * node types it called leaves, which is a renderer's distinction -- a text
     * node has no closing tag, so the event was waste for every renderer cmark
     * ships. It was never a structural one: a non-leaf type with no children
     * got an exit anyway, so the rule was "my type is on a list", not "nothing
     * is below me". A consumer asking the structural question -- am I finished
     * with this node -- had to carry that list around to ask it. */
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

markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter) { return iter->cur.node; }

int markdown_core_node_consolidate_texts(markdown_core_node *root) {
    return markdown_core_node_consolidate_texts_from(root, NULL);
}

/* THE SAME WALK, OVER THE FRONTIER ONLY. A refine that kept a unit's
 * settled prefix must not walk it again — that walk is what makes a tick
 * cost the whole leaf — and it need not: the prefix was consolidated when
 * it settled, and a settle point is a LINE START, so the child before the
 * frontier is a break and never a Text that could merge across it. */
int markdown_core_node_consolidate_texts_from(markdown_core_node *root, markdown_core_node *after) {
    if (root == NULL) {
        return 1;
    }
    if (after) {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(root->content.mem);
        markdown_core_node *child = after->next;
        int ok = 1;
        while (child) {
            markdown_core_node *next = child->next;
            /* Each new child's own subtree, whole — they are this refine's
             * and none of them is the settled prefix. */
            if (child->first_child && !markdown_core_node_consolidate_texts(child)) {
                ok = 0;
            }
            /* And the run itself, merged in place: never across `after`,
             * whose bytes are settled and whose successor at a settle point
             * is a line break in any case. */
            if (child->type == MARKDOWN_CORE_NODE_TEXT && next && next->type == MARKDOWN_CORE_NODE_TEXT) {
                markdown_core_strbuf_clear(&buf);
                markdown_core_strbuf_put(&buf, child->as.literal.data, child->as.literal.len);
                while (next && next->type == MARKDOWN_CORE_NODE_TEXT) {
                    markdown_core_node *after_next = next->next;
                    markdown_core_strbuf_put(&buf, next->as.literal.data, next->as.literal.len);
                    child->end_column = next->end_column;
                    markdown_core_node_free(next);
                    next = after_next;
                }
                markdown_core_chunk_free(root->content.mem, &child->as.literal);
                child->as.literal = markdown_core_chunk_buf_detach(&buf);
                if (!child->as.literal.data) {
                    ok = 0;
                }
            }
            child = next;
        }
        markdown_core_strbuf_free(&buf);
        return ok;
    }
    {
        markdown_core_iter *iter = markdown_core_iter_new(root);
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(root->content.mem);
        markdown_core_event_type ev_type;
        markdown_core_node *cur, *tmp, *next;
        int ok = 1;

        if (!iter) {
            return 0;
        }

        while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            cur = markdown_core_iter_get_node(iter);
            if (ev_type == MARKDOWN_CORE_EVENT_ENTER && cur->type == MARKDOWN_CORE_NODE_TEXT && cur->next &&
                cur->next->type == MARKDOWN_CORE_NODE_TEXT) {
                markdown_core_strbuf_clear(&buf);
                markdown_core_strbuf_put(&buf, cur->as.literal.data, cur->as.literal.len);
                tmp = cur->next;
                while (tmp && tmp->type == MARKDOWN_CORE_NODE_TEXT) {
                    markdown_core_strbuf_put(&buf, tmp->as.literal.data, tmp->as.literal.len);
                    cur->end_column = tmp->end_column;
                    next = tmp->next;
                    markdown_core_node_free(tmp);
                    tmp = next;
                }
                /* Every node the iterator could still be pointing at has just been
                 * freed, so the successor it computed before handing us this ENTER
                 * is stale. Re-seat on `cur`, the one node in the run that
                 * survives: the walk recomputes from a sibling chain that now ends
                 * the run at `cur`, and the re-delivered ENTER falls straight
                 * through the guard above because `cur->next` is no longer TEXT. */
                markdown_core_iter_reset(iter, cur, MARKDOWN_CORE_EVENT_ENTER);
                markdown_core_chunk_free(iter->mem, &cur->as.literal);
                cur->as.literal = markdown_core_chunk_buf_detach(&buf);
                if (!cur->as.literal.data) {
                    ok = 0;
                }
            }
        }

        markdown_core_strbuf_free(&buf);
        markdown_core_iter_free(iter);
        return ok;
    }
}
