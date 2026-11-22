#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "buffer.h"
#include "inline_internal.h"

/* The inline state the preparation's first pass leaves for the heading of
 * the names' entry `at` (heading.c, markdown_core_headings_prepare), which
 * its second pass parses to the end. */
typedef struct {
    size_t at;
    markdown_core_inline_state *state;
} markdown_core_heading_held;

/* THE HEADING ELEMENT'S PARSE RECORD: the inline states the preparation
 * under way leaves waiting, in entry order; the stack the projections of a
 * heading's text use in turn (heading.c, heading_project), and what
 * measuring a long heading's children projects them to (heading_measure).
 * The headings themselves and their names are the names service's
 * (names.h). */
typedef struct {
    markdown_core_heading_held *held;
    size_t held_count, held_room;
    void *stack;
    size_t stack_room;
    markdown_core_strbuf scratch;
} markdown_core_heading_state;

/* ONE HEADING'S INLINE RUN (the heading element's run record): where the
 * text a reference label is taken from ends. Zero in every other run. */
typedef struct {
    bufsize_t label_end;
} markdown_core_heading_run;

#endif
