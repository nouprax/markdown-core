#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "facts.h"
#include "inline_internal.h"

/* A heading the parse read, registered once when its block closes. Source
 * order is settled before its inlines are parsed, independently of the
 * order in which mapped inputs close. Pending holds the ordinary inline
 * cursor at its declaration dependency; `fact` is what it declares to the
 * registries (registry.h), which hold it. */
typedef struct {
    markdown_core_node *node;
    /* Where the heading starts, recorded as it closes. */
    uint64_t start;
    markdown_core_inline_state *pending;
    markdown_core_fact *fact;
} markdown_core_heading_parse;

typedef struct {
    markdown_core_heading_parse *values;
    size_t count, capacity;
} markdown_core_heading_collection;

/* THE HEADINGS OF ONE PARSE (the heading element's parse record): each
 * heading the parse read, as its block closed. */
typedef struct {
    markdown_core_heading_collection headings;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where
 * its trailing attributes begin, -1 when it has none, and where the text a
 * reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t attributes_start, label_end;
} markdown_core_heading_run;

#endif
