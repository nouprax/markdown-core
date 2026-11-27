#ifndef MARKDOWN_CORE_HEADING_STATE_H
#define MARKDOWN_CORE_HEADING_STATE_H
#include "buffer.h"
#include "inline_internal.h"

/* THE HEADING ELEMENT'S PARSE RECORD: the stack the projections of a
 * heading's text use in turn (heading.c, heading_project), and what
 * measuring a heading's children projects them to (heading_measure).
 * The headings themselves and their names are the names service's
 * (names.h). */
typedef struct {
    void *stack;
    size_t stack_room;
    markdown_core_strbuf scratch;
} markdown_core_heading_state;

#endif
