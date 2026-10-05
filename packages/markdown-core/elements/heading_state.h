#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "references.h"
#include "inline_internal.h"

/* A heading is registered once when its block closes. Source order is settled
 * before resolution, independently of the order in which mapped inputs close.
 * Pending holds the ordinary inline cursor at its declaration dependency;
 * nodes remain owned by the tree. */
typedef struct {
    markdown_core_node *node;
    markdown_core_inline_state *pending;
} markdown_core_heading_parse;

typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity;
} markdown_core_heading_collection;

typedef struct {
    markdown_core_key_index index;
} anchor_registry;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): each
 * heading as its block closed, the anchors the document reserves and assigns
 * once the tree is complete, and the projection and registry work that
 * assignment did, for its complexity gate. */
typedef struct {
    markdown_core_heading_collection headings;
    anchor_registry anchors;
    size_t anchor_work;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
