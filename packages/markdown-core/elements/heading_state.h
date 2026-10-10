#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "map.h"
#include "inline_internal.h"

/* A heading is taken from the parser's inline roots as the document is
 * prepared, with the builder of its root, which its content is parsed into.
 * Source order is settled before resolution, independently of the order in
 * which mapped inputs close. Pending holds the ordinary inline cursor at its
 * declaration dependency. */
typedef struct {
    markdown_core_source_entry source;
    markdown_core_member *builder;
    markdown_core_inline_state *pending;
} markdown_core_heading_parse;

typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity;
} markdown_core_heading_collection;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): each
 * heading the parse made, as its block closed or as it was parsed again in
 * place, and the projection and family work their anchors took, for its
 * complexity gate. */
typedef struct {
    markdown_core_heading_collection headings;
    /* How many of the parser's inline roots the headings were taken from. */
    size_t roots;
    size_t anchor_work;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
