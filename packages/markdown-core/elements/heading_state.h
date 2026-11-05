#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "zipper.h"
#include "inline_internal.h"

/* A heading is registered once when its block closes, with where it starts.
 * Source order is settled before resolution, independently of the order in
 * which mapped inputs close. Pending holds the ordinary inline cursor at its
 * declaration dependency while the preparation parses it; the node's
 * HEADING_PARSED says the heading has declared its label and its inline
 * content is parsed, which a completion does once, for the headings that
 * settled since the last (markdown_core_headings_prepare), and which a
 * reading put back to a mark takes back with the node; nodes remain owned
 * by the tree, and an entry names its node, which keeps its address for its
 * whole life. The parse writes the entry's fields after `start` anew.
 * `authored` says its attributes wrote its anchor. `name` is the anchor the headings computed
 * for it, the node's own bytes, `name_size` of them, of which the first
 * `base_size` are the base its text projects to; NULL when it holds none,
 * or when its inline content was made again since. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
    const unsigned char *name;
    bufsize_t base_size, name_size;
    uint32_t start;
    bool authored;
    /* A heading a streaming commit named whose node a snapshot gave another
     * name (heading.c, A COMMITTED HEADING UNDER ANOTHER NAME): `holds_name`
     * says the entry owns the bytes of `name`, which the node no longer
     * shows; `renamed` says the snapshot under way gave it its name. */
    bool holds_name, renamed;
} markdown_core_heading_parse;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record).
 *
 * First, what a mark keeps of them (element.h, `kept_size`): `count`, the
 * headings the reading so far closed, and `parsed`, of which the first are
 * parsed and in source order, before every heading after them. A reading put
 * back to a mark finds the headings the mark found, the entries after them
 * gone and the pending block's, the one entry a commit leaves not parsed,
 * naming its block as the mark found it.
 *
 * The headings themselves are a zipper (zipper.h) about an edit's rest
 * point: its front holds the reading's, in the order they closed, `count`
 * of them, which the front's own count follows once heading.c next reads
 * them (heading_sync); its back the old tail's, after the rest point, which
 * the reading takes back at its splice (`spliced`) or lets go at its finish.
 * A parse no edit reopened keeps no back.
 *
 * Then the committed names (heading.c, THE COMMITTED NAMES): the names
 * taken, where the reading of the roster's ANCHOR records stands and how
 * many it read, how many headings are named, and the committed headings
 * that show another name, from the last snapshot and then from the one
 * under way; and the projection and lookup work the assignment of their
 * anchors did, for its complexity gate. */
typedef MARKDOWN_CORE_ZIPPER(markdown_core_heading_parse) markdown_core_heading_zipper;

/* THE STEMS (heading.c, THE NAMES AN EDIT MOVES): a name that is the base
 * of a heading's anchor, its `key` the stem's own bytes, `size` of them;
 * where each heading of that base starts, a zipper about the headings'
 * focus, its back measured from their end; the present stems whose parent
 * it is, the stem's own name less one suffix `-k` of its end, linked
 * through `first`, then `next` and `prior`, each a stem's place plus one
 * or zero; what an edit's pass of the names last saw it in, the pass an
 * anchor the edit made or let go names it for, and the suffix that pass
 * tries next for its headings. */
typedef MARKDOWN_CORE_ZIPPER(uint32_t) markdown_core_heading_starts;

typedef struct {
    markdown_core_heading_starts starts;
    unsigned char *key;
    bufsize_t size;
    uint32_t first, next, prior, seen, anchored;
    size_t suffix;
} markdown_core_heading_stem;

/* WHAT AN EDIT KEEPS OF THE NAMES (heading.c, THE NAMES AN EDIT MOVES):
 * every stem, those free chained from `spare` through `next`, and the index
 * of their keys; where the edit's reading starts and its first heading;
 * the pass under way, the stems the edit touched, to name again, and the
 * names of the headings it let go. */
typedef struct markdown_core_heading_names {
    markdown_core_heading_stem *stems;
    size_t stem_count, stem_room;
    uint32_t spare;
    markdown_core_key_index index;
    uint32_t rest;
    size_t fresh;
    uint32_t pass;
    uint32_t *touched;
    size_t touched_count, touched_room;
    /* The names of the headings the edit let go, each its stem's place and
     * size, four bytes each, then its bytes. */
    markdown_core_strbuf lost;
} markdown_core_heading_names;

typedef struct {
    size_t count, parsed;
    markdown_core_heading_zipper headings;
    bool spliced;
    markdown_core_key_index taken;
    size_t records_at, records_read, named;
    uint32_t *renamed;
    size_t renamed_count, renamed_capacity;
    size_t anchor_work;
    /* What only an edit keeps of the names, made at its first reopen. */
    struct markdown_core_heading_names *names;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
