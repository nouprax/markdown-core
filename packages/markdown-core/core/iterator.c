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

static bool S_is_leaf(markdown_core_node *node) {
    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_SOFT_BREAK:
    case MARKDOWN_CORE_NODE_LINE_BREAK:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML:
        return 1;
    }
    return 0;
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
    if (ev_type == MARKDOWN_CORE_EVENT_ENTER && !S_is_leaf(node)) {
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

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && cur->type == MARKDOWN_CORE_NODE_TEXT && cur->next &&
            cur->next->type == MARKDOWN_CORE_NODE_TEXT) {
            markdown_core_strbuf_clear(&buf);
            markdown_core_strbuf_put(&buf, cur->as.literal.data, cur->as.literal.len);
            tmp = cur->next;
            while (tmp && tmp->type == MARKDOWN_CORE_NODE_TEXT) {
                markdown_core_iter_next(iter); // advance pointer
                markdown_core_strbuf_put(&buf, tmp->as.literal.data, tmp->as.literal.len);
                cur->end_column = tmp->end_column;
                next = tmp->next;
                markdown_core_node_free(tmp);
                tmp = next;
            }
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
