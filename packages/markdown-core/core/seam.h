#ifndef MARKDOWN_CORE_SEAM_H
#define MARKDOWN_CORE_SEAM_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "node.h"
#include "parser.h"
#include "slab.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE SEAMS (docs/plans/huge-paragraphs.md): where an edit's reading of the
 * new text and the old tree can be sewn together. A seam is a place between
 * two children of a holder -- the root, a block of the chain the reading
 * reopened, or a paragraph's inline content -- where the reading of the
 * holder stands in its empty state, so that what it made before the seam
 * reads no byte past it. Each holder's reading does the same four steps:
 *
 * - REST: it finds the last seam two lines or more before the edit
 *   (elements/session.c, session_rest and session_rest_in, for the root and
 *   the blocks of the chain; blocks.c, S_seam_pick, for a paragraph).
 * - KEEP: it keeps the children before the seam, and the old ones past it
 *   stay in the holder's own storage, the OLD TAIL (THE OLD TAIL IN PLACE).
 *   A child the reading makes that takes the place of an old one in place
 *   moves that one out to the edit's room for it first (markdown_core_seam_out).
 * - MEETS: past the edit, at each place it stands empty again, it asks
 *   whether it stands where the old reading stood at one of the old tail's
 *   seams (blocks.c, S_resumes, S_resumes_in and S_seam_meets): each holder
 *   asks of its own state, which no other holder's reading knows.
 * - SPLICE: it takes the old tail back from that seam on, after the
 *   children it made (markdown_core_seam_join), and stops.
 *
 * The root and the blocks of the chain measure the old tail's places by the
 * edit's delta and their children's widths; a paragraph's seams past the
 * edit are measured back from the paragraph's ends (markdown_core_seam_flip
 * and markdown_core_seam_sum). */

/* The old tail `tail`'s children from `tail->saved` up to `end`, which are
 * still in place in the holder's storage `items`, move out to its `room`,
 * as the storage is about to be written over or to go. */
static inline void markdown_core_seam_out(markdown_core_node **room, markdown_core_node *const *items,
                                          markdown_core_seam_tail *tail, uint32_t end) {
    if (tail->saved < end) {
        memcpy(room + tail->base + tail->saved, items + tail->start + tail->saved, (end - tail->saved) * sizeof(*room));
        tail->saved = end;
    }
}

/* The storage of a holder whose first `at` children are those it keeps and
 * the reading made, with the old tail `tail` after them from its child
 * `first` on: `gap` places for children still to come, then those of it the
 * `room` holds, then those still in place, moved by one memmove, or copied
 * into new storage when `items` holds too few. The holder's storage is
 * `items`, or the new storage that replaced it; NULL, with the storage as
 * it was, when storage could not be had. */
static inline markdown_core_node **markdown_core_seam_join(markdown_core_node_pool *pool, markdown_core_node **items,
                                                           uint32_t at, uint32_t gap,
                                                           const markdown_core_seam_tail *tail,
                                                           markdown_core_node *const *room, uint32_t first) {
    assert(first <= tail->saved && tail->saved <= tail->held);
    const uint32_t count = tail->saved - first, from = tail->start + tail->saved, kept = tail->held - tail->saved;
    const uint32_t total = at + gap + count + kept, to = at + gap + count;
    if (!items || markdown_core_bytes_size(items) < (size_t)total * sizeof(*items)) {
        markdown_core_node **grown = (markdown_core_node **)markdown_core_node_pool_bytes(
            pool, (size_t)markdown_core_node_array_capacity(total) * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        if (items) {
            memcpy(grown, items, at * sizeof(*grown));
            memcpy(grown + to, items + from, kept * sizeof(*grown));
            markdown_core_node_pool_bytes_free(pool, items);
        }
        items = grown;
    } else if (to != from && kept) {
        memmove(items + to, items + from, kept * sizeof(*items));
    }
    if (count) {
        memcpy(items + at + gap, room + tail->base + first, count * sizeof(*items));
    }
    return items;
}

/* The holder whose storage is `*items`, `*count` children the reading gave
 * it, takes its old tail `tail` back from its child `first` on, after them:
 * those the reading passed, which their records will own, move out first,
 * the ones still in place move to follow the reading's, and those moved out
 * before are copied back ahead of them; none is in place after. False, with
 * the holder as it was, when storage could not be had. */
static inline bool markdown_core_seam_mend(markdown_core_node_pool *pool, markdown_core_node ***items, uint32_t *count,
                                           markdown_core_seam_tail *tail, markdown_core_node **room, uint32_t first) {
    assert(first <= tail->held);
    markdown_core_seam_out(room, *items, tail, first);
    if (first == tail->held) {
        return true;
    }
    markdown_core_node **mended = markdown_core_seam_join(pool, *items, *count, 0, tail, room, first);
    if (!mended) {
        return false;
    }
    *items = mended;
    *count += tail->held - first;
    tail->saved = tail->held;
    return true;
}

/* A paragraph's seam measured back from its `ends`, as the back holds it,
 * or one the back holds measured as the front does: each of its places
 * from the other end (blocks.c, THE OLD TAIL OF A PARAGRAPH). */
static inline markdown_core_seam markdown_core_seam_flip(markdown_core_seam seam, const markdown_core_seam *ends) {
    seam.offset = ends->offset - seam.offset;
    seam.index = ends->index - seam.index;
    seam.uses = ends->uses - seam.uses;
    seam.line = ends->line - seam.line;
    seam.source = ends->source - seam.source;
    seam.place = ends->place - seam.place;
    seam.read = ends->read - seam.read;
    return seam;
}

/* The front's seam `seam` as the back holds it: its measure is of the
 * children after it. */
static inline markdown_core_seam markdown_core_seam_back(markdown_core_seam seam, const markdown_core_seam *ends) {
    const markdown_core_measure measure = markdown_core_measure_back(seam.measure, ends->measure);
    seam = markdown_core_seam_flip(seam, ends);
    seam.measure = measure;
    return seam;
}

/* The back's seam `seam` as the front holds it: its measure is of the
 * children before it. */
static inline markdown_core_seam markdown_core_seam_front(markdown_core_seam seam, const markdown_core_seam *ends) {
    const markdown_core_measure measure = markdown_core_measure_front(seam.measure, ends->measure);
    seam = markdown_core_seam_flip(seam, ends);
    seam.measure = measure;
    return seam;
}

/* The ends of a paragraph whose seam `seam` the back measures as `back`. */
static inline markdown_core_seam markdown_core_seam_sum(markdown_core_seam seam, markdown_core_seam back) {
    return (markdown_core_seam){seam.offset + back.offset,
                                seam.index + back.index,
                                seam.uses + back.uses,
                                seam.line + back.line,
                                seam.source + back.source,
                                seam.place + back.place,
                                seam.read + back.read,
                                0,
                                markdown_core_measure_join(seam.measure, back.measure)};
}

#ifdef __cplusplus
}
#endif

#endif
