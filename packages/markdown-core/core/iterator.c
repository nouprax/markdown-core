#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "node_type.h"
#include "buffer.h"
#include "parser.h"
#include "iterator.h"

void markdown_core_iter_reset(markdown_core_iter *iter, markdown_core_member *current,
                              markdown_core_event_type event_type) {
    iter->next.ev_type = event_type;
    iter->next.member = current;
    markdown_core_iter_step(iter);
}

/* The surviving Text owns the concatenated literal and a concatenation of
 * its operands' source runs. A caller outside a parse has no parser-owned
 * map to retain and uses the public entry point with NULL.
 *
 * EXIT, not ENTER, and that is Step 5's mutation rule: the only node a walk
 * may free is the one whose EXIT is current. `TEXT` was in the old
 * `S_is_leaf` list, so its EXIT was suppressed and freeing at ENTER
 * happened to be safe; with the contract total it is a use-after-free. */
markdown_core_complete_result markdown_core_consolidate_text_step(markdown_core_parser *parser,
                                                                  markdown_core_iter *iter, markdown_core_member *text,
                                                                  markdown_core_complete_node_func complete,
                                                                  int depth) {
    markdown_core_member *member, *next;
    markdown_core_node *cur = text->node, *tmp;

    assert(iter->cur.member == text && iter->cur.ev_type == MARKDOWN_CORE_EVENT_EXIT);
    assert(cur->kind == MARKDOWN_CORE_NODE_TEXT);

    if (text->next && text->next->node->kind == MARKDOWN_CORE_NODE_TEXT) {
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
        for (member = text->next; member && member->node->kind == MARKDOWN_CORE_NODE_TEXT; member = member->next) {
            tmp = member->node;
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
            return MARKDOWN_CORE_COMPLETE_FAILED;
        }
        if (view) {
            combined_map.first = cur->content_map.first;
            combined_map.count = view_end - cur->content_map.first;
            combined_map.offset = cur->content_map.offset;
        } else if (parser && !markdown_core_parser_append_content_marks(parser, &cur->content_map, &combined_map, 0,
                                                                        cur->as.literal->len, 0)) {
            return MARKDOWN_CORE_COMPLETE_FAILED;
        }
        unsigned char *merged = markdown_core_bytes_take(NULL, length + 1, 0);
        if (!merged) {
            return MARKDOWN_CORE_COMPLETE_FAILED;
        }
        bufsize_t at = cur->as.literal->len;
        if (at) {
            memcpy(merged, cur->as.literal->data, (size_t)at);
        }
        member = text->next;
        while (member && member->node->kind == MARKDOWN_CORE_NODE_TEXT) {
            tmp = member->node;
            /* Bring `tmp` to its own EXIT before freeing it: two events now,
             * where a suppressed EXIT used to make one enough. They are steps
             * of the walk this is part of, taken HERE so that no step after
             * this one is ever handed a node this one is about to free. */
            markdown_core_iter_step(iter); /* ENTER */
            markdown_core_iter_step(iter); /* EXIT  */
            if (complete) {
                complete(parser, tmp, depth);
            }
            if (parser && !view &&
                !markdown_core_parser_append_content_marks(parser, &tmp->content_map, &combined_map, 0,
                                                           tmp->as.literal->len, at)) {
                markdown_core_bytes_release(NULL, merged);
                return MARKDOWN_CORE_COMPLETE_FAILED;
            }
            if (tmp->as.literal->len) {
                memcpy(merged + at, tmp->as.literal->data, (size_t)tmp->as.literal->len);
                at += tmp->as.literal->len;
            }
            // THE MERGED TEXT COVERS EVERY OPERAND'S SCOPE, an empty one's
            // too: the operands lie left to right, each where the previous
            // one ended, and an empty one still lies on the bytes it was
            // made of, as the spaces a line ending trims out of a slice's
            // literal (text.c). The Text a slice ending at
            // that line ending makes covers them, so the Text merged from
            // slices cut anywhere else covers them as well: a scope does not
            // depend on where the tokens were cut.
            cur->where.place.end = tmp->where.place.end;
            next = member->next;
            markdown_core_member_unlink(member);
            markdown_core_parser_release_member(parser, member);
            member = next;
        }
        /* Every node the loop freed was ahead of the cursor and is now
         * unlinked, so the cursor sits at the last one's EXIT. Re-establish
         * `cur`'s EXIT: it recomputes the lookahead from the siblings that
         * survived, and it is what makes the drop below legal under the
         * rule rather than merely safe. It is not a step of the walk: the
         * event it re-delivers was delivered already. */
        if (parser) {
            cur->content_map = combined_map;
        }
        markdown_core_iter_reset(iter, text, MARKDOWN_CORE_EVENT_EXIT);
        if (cur->as.literal->alloc) {
            markdown_core_node_pool_bytes_free(parser ? parser->pool : NULL, cur->as.literal->data);
        }
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
        markdown_core_member_unlink(text);
        markdown_core_parser_release_member(parser, text);
        return MARKDOWN_CORE_COMPLETE_CONSUMED;
    }
    return MARKDOWN_CORE_COMPLETE_CONTINUE;
}
