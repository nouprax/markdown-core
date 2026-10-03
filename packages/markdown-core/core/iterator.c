#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "node_type.h"
#include "buffer.h"
#include "parser.h"
#include "iterator.h"

bool markdown_core_iter_path_reserve(markdown_core_iter_path *path) {
    size_t capacity = path->capacity ? 2 * path->capacity : 32;
    if (capacity > SIZE_MAX / sizeof(*path->frames)) {
        return false;
    }
    markdown_core_iter_frame *frames = markdown_core_realloc(path->frames, capacity * sizeof(*frames));
    if (!frames) {
        return false;
    }
    path->frames = frames;
    path->capacity = capacity;
    return true;
}

void markdown_core_iter_path_dispose(markdown_core_iter_path *path) {
    markdown_core_free(path->frames);
    *path = (markdown_core_iter_path){0};
}

markdown_core_iter *markdown_core_iter_new(markdown_core_node *root) {
    if (root == NULL) {
        return NULL;
    }
    markdown_core_iter *iter = (markdown_core_iter *)markdown_core_alloc(1, sizeof(markdown_core_iter));
    if (!iter) {
        return NULL;
    }
    markdown_core_iter_init(iter, &iter->own, root);
    return iter;
}

void markdown_core_iter_free(markdown_core_iter *iter) {
    if (iter) {
        markdown_core_iter_path_dispose(&iter->own);
        markdown_core_free(iter);
    }
}

markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter) { return markdown_core_iter_step(iter); }

markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter) { return markdown_core_iter_node(iter); }

bool markdown_core_iter_insert_before(markdown_core_iter *iter, markdown_core_node_pool *pool,
                                      markdown_core_node *node) {
    markdown_core_iter_frame *parent = &iter->path->frames[iter->path->count - 2];
    if (!markdown_core_children_insert(pool, &parent->node->children, parent->at, node)) {
        return false;
    }
    parent->at++;
    return true;
}

bool markdown_core_iter_take_current(markdown_core_iter *iter, markdown_core_node_pool *pool,
                                     markdown_core_node **taken) {
    assert(iter->event == MARKDOWN_CORE_EVENT_EXIT);
    markdown_core_iter_frame *parent = &iter->path->frames[iter->path->count - 2];
    if (!markdown_core_children_remove(pool, &parent->node->children, parent->at, taken)) {
        return false;
    }
    /* The step after this EXIT moves the parent past the child it was in,
     * which is now the next sibling's index. */
    parent->at--;
    return true;
}

bool markdown_core_iter_take_next(markdown_core_iter *iter, markdown_core_node_pool *pool, markdown_core_node **taken) {
    markdown_core_iter_frame *parent = &iter->path->frames[iter->path->count - 2];
    return markdown_core_children_remove(pool, &parent->node->children, parent->at + 1, taken);
}

/* The surviving Text owns the concatenated literal and a concatenation of
 * its operands' source runs. A caller outside a parse has no parser-owned
 * map to retain and uses the public entry point with NULL.
 *
 * EXIT, not ENTER: the only node a walk takes out of the tree is the one
 * whose EXIT is current, and the siblings after it, which the walk has not
 * reached. */
markdown_core_finish_result markdown_core_consolidate_text_step(markdown_core_parser *parser, markdown_core_iter *iter,
                                                                markdown_core_node *cur,
                                                                markdown_core_complete_node_func complete, int depth) {
    markdown_core_node_pool *pool = parser ? parser->pool : NULL;
    markdown_core_iter_frame *parent = &iter->path->frames[iter->path->count - 2];
    const markdown_core_run *siblings = parent->node->children;
    size_t total = markdown_core_children_count(siblings);

    assert(markdown_core_iter_node(iter) == cur && iter->event == MARKDOWN_CORE_EVENT_EXIT);
    assert(cur->kind == MARKDOWN_CORE_NODE_TEXT);

    /* The Text siblings after `cur`: the run is [parent->at + 1, end). */
    size_t end = parent->at + 1;
    while (end < total && markdown_core_children_at(siblings, end)->kind == MARKDOWN_CORE_NODE_TEXT) {
        end++;
    }
    if (end > parent->at + 1) {
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
        for (size_t i = parent->at + 1; i < end; i++) {
            const markdown_core_node *tmp = markdown_core_children_at(siblings, i);
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
        for (size_t left = end - parent->at - 1; left; left--) {
            markdown_core_node *tmp = markdown_core_iter_next_sibling(iter);
            if (complete) {
                complete(parser, tmp, depth);
            }
            if (parser && !view &&
                !markdown_core_parser_append_content_marks(parser, &tmp->content_map, &combined_map, 0,
                                                           tmp->as.literal->len, at)) {
                markdown_core_free(merged);
                return MARKDOWN_CORE_FINISH_FAILED;
            }
            if (!markdown_core_iter_take_next(iter, pool, &tmp)) {
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
            // characters.
            if (tmp->as.literal->len > 0) {
                cur->where.place.end = tmp->where.place.end;
            }
            markdown_core_parser_release_node(parser, tmp);
        }
        if (parser) {
            cur->content_map = combined_map;
        }
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
    // Taking it out is legal because `cur`'s EXIT is current. The caller
    // learns that `cur` is gone and hands this event to nothing else.
    if (cur->as.literal->len == 0) {
        if (!markdown_core_iter_take_current(iter, pool, &cur)) {
            return MARKDOWN_CORE_FINISH_FAILED;
        }
        markdown_core_parser_release_node(parser, cur);
        return MARKDOWN_CORE_FINISH_CONSUMED;
    }
    return MARKDOWN_CORE_FINISH_CONTINUE;
}
