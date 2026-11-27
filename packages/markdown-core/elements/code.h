#ifndef MARKDOWN_CORE_ELEMENT_CODE_H
#define MARKDOWN_CORE_ELEMENT_CODE_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE;
/* A BACKTICK INDEX (the code element's run record): the last position of
 * a run of each length up to `capacity`, recorded as a reading for a
 * closer passes it, and `from`, the earliest place a reading for a closer
 * read on from to the end of the run (UINT32_MAX while none did): from
 * there on, a length whose last run lies behind a place has no closer
 * after it, wherever the readings started and in whatever order. Then
 * what one reading looks for: the closer's length, and where it ends.
 * Allocated when its first reading looks for a closer. */
typedef struct {
    bufsize_t *positions;
    bufsize_t capacity;
    uint32_t from;
    uint32_t want, close;
} markdown_core_backticks;
#endif
