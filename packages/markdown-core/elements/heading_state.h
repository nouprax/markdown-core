#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "inline_internal.h"

/* A heading is registered once when its block closes. Source order is settled
 * before resolution, independently of the order in which mapped inputs close.
 * Pending holds the ordinary inline cursor at its declaration dependency;
 * `parsed` says the heading has declared its label and its inline content is
 * parsed, which a completion does once, for the headings that settled since
 * the last (markdown_core_headings_prepare); nodes remain owned by the tree. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
    bool parsed;
} markdown_core_heading_parse;

/* The headings in the order they closed, and `parsed`, how many come first
 * that are parsed and in source order, before every heading after them. */
typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity, parsed;
} markdown_core_heading_collection;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): each
 * heading as its block closed, and the projection and lookup work the
 * assignment of their anchors did, for its complexity gate. */
typedef struct {
    markdown_core_heading_collection headings;
    size_t anchor_work;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
