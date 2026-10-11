#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "inline_internal.h"

/* A heading whose text is parsed as its root completes, with the builder of
 * its root, which its content is parsed into. Pending holds the ordinary
 * inline cursor at its declaration dependency. */
typedef struct {
    markdown_core_source_entry source;
    markdown_core_member *builder;
    markdown_core_inline_state *pending;
} markdown_core_heading_parse;

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

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
