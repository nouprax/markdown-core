#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "inline_internal.h"

/* A heading is registered once when its block closes, with where it starts.
 * Source order is settled before resolution, independently of the order in
 * which mapped inputs close. Pending holds the ordinary inline cursor at its
 * declaration dependency; `parsed` says the heading has declared its label
 * and its inline content is parsed, which a completion does once, for the
 * headings that settled since the last (markdown_core_headings_prepare);
 * nodes remain owned by the tree, and an entry names its node, which keeps
 * its address for its whole life. `authored` says its
 * attributes wrote its anchor. `name` is the anchor the headings computed
 * for it, the node's own bytes, `name_size` of them, of which the first
 * `base_size` are the base its text projects to; NULL when it holds none,
 * or when its inline content was made again since. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
    const unsigned char *name;
    bufsize_t base_size, name_size;
    uint32_t start;
    bool parsed, authored;
    /* A heading a streaming commit named whose node a snapshot gave another
     * name (heading.c, A COMMITTED HEADING UNDER ANOTHER NAME): `holds_name`
     * says the entry owns the bytes of `name`, which the node no longer
     * shows; `renamed` says the snapshot under way gave it its name. */
    bool holds_name, renamed;
} markdown_core_heading_parse;

/* The headings in the order they closed, and `parsed`, how many come first
 * that are parsed and in source order, before every heading after them. */
typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity, parsed;
} markdown_core_heading_collection;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): each
 * heading as its block closed, the collection's count and how many were
 * parsed as the snapshot being taken found them
 * (markdown_core_headings_start_snapshot), with the registry's length then;
 * the committed names (heading.c, THE COMMITTED NAMES): the names taken,
 * where the reading of the registry's ANCHOR records stands and how many it
 * read, how many headings are named, and the committed headings that show
 * another name, from the last snapshot and then from the one under way; and
 * the projection and lookup work the assignment of their anchors did, for
 * its complexity gate. */
typedef struct {
    markdown_core_heading_collection headings;
    size_t snapshot_count, snapshot_parsed, snapshot_records;
    markdown_core_key_index taken;
    size_t records_at, records_read, named;
    uint32_t *renamed;
    size_t renamed_count, renamed_capacity;
    size_t anchor_work;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
