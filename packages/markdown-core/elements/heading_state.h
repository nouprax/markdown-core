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
 * `authored` says its attributes wrote its anchor. `named` says its node
 * holds the base its inline content projects to (node.h, the heading's
 * name), which its inline content made again since does not; `suffix` is
 * the suffix of the name the headings committed for it, which its node
 * shows but while a snapshot gives it another (heading.c, A COMMITTED
 * HEADING UNDER ANOTHER NAME), and `renamed` says the snapshot under way
 * did. `stem` is its base's stem, its place plus one, once an edit keeps
 * them (heading.c, THE NAMES AN EDIT MOVES). */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
    uint32_t start, suffix, stem;
    bool authored, named, renamed;
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
 * of a heading's anchor, kept as its `measure` alone, its bytes those its
 * witness holds (heading.c, stem_text): its first heading's base, else its
 * first child's bytes, else, once it holds neither, the bytes of what it
 * lost last: the base its last heading to go had before the edit, `relic`,
 * or its last child to go, `heir`. Where each heading of that base starts,
 * a zipper about the headings' focus, its back measured from their end; the
 * stem's parent, `up`, the stem's own name less one suffix `-k` of its end,
 * and the present stems whose parent it is, linked through `first`, then
 * `next` and `prior`, each a stem's place plus one or zero; what an edit's
 * pass of the names last saw it in, the pass an anchor the edit made or let
 * go names it for, and the suffix that pass tries next for its headings. A
 * stem whose measure is empty is free. */
typedef MARKDOWN_CORE_ZIPPER(uint32_t) markdown_core_heading_starts;

typedef struct {
    markdown_core_heading_starts starts;
    markdown_core_measure measure;
    const markdown_core_strbuf *relic;
    uint32_t up, first, next, prior, heir, seen, anchored;
    size_t suffix;
} markdown_core_heading_stem;

/* What an edit lets go of a heading: its stem and the suffix of its name. */
typedef struct {
    uint32_t stem, suffix;
} markdown_core_heading_lost;

/* THE BASE A HEADING THE EDIT READ TAKES, before the edit's end (heading.c,
 * THE NEW BASES): the bytes of `old`, the base its node had, but for those
 * from `front` to `back` before its end, which `mid` takes the place of; or,
 * with no `old`, `mid` whole. It measures `measure`. */
typedef struct {
    const markdown_core_strbuf *old;
    bufsize_t front, back;
    markdown_core_strbuf mid;
    markdown_core_measure measure;
} markdown_core_heading_base;

/* The base of a node an edit made a paragraph again (heading.c,
 * heading_revert), kept to its end. */
typedef struct {
    const markdown_core_node *node;
    markdown_core_strbuf *base;
} markdown_core_heading_retired;

/* WHAT AN EDIT KEEPS OF THE NAMES (heading.c, THE NAMES AN EDIT MOVES):
 * every stem, those free chained from `spare` through `next`, and the index
 * of their names; where the edit's reading starts and its first heading;
 * the pass under way, the stems the edit touched, to name again, what the
 * headings it let go held, and the new bases of the headings it read, one
 * for each from `fresh` on; and the bases of the nodes the edit made
 * paragraphs again (heading_revert), which their stems' relics read until
 * the edit's end. */
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
    markdown_core_heading_lost *lost;
    size_t lost_count, lost_room;
    markdown_core_heading_base *bases;
    size_t base_count, base_room;
    markdown_core_heading_retired *retired;
    size_t retired_count, retired_room;
    /* What measuring a long heading's children projects them to, and the
     * projection's stack (heading.c, heading_measure). */
    markdown_core_strbuf scratch;
    void *stack;
    size_t stack_room;
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
