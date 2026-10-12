#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "inline_internal.h"

typedef enum { ANCHOR_CONTENT, ANCHOR_CITATIONS, ANCHOR_KEY } anchor_projection_kind;
/* The nodes left to project at one level: those of `stem` from `index` on,
 * or, for a key, the citation `node`. */
typedef struct {
    const markdown_core_stem *stem;
    size_t index;
    markdown_core_node *node;
    anchor_projection_kind kind;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): the
 * storage each heading's anchor base is projected in, which serves every
 * heading of the parse, and the projection and family work their anchors
 * took, for its complexity gate. */
typedef struct {
    markdown_core_strbuf base;
    anchor_projection_stack projections;
    size_t anchor_work;
} markdown_core_heading_state;

#endif
