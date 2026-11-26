#ifndef MARKDOWN_CORE_ELEMENT_CODE_H
#define MARKDOWN_CORE_ELEMENT_CODE_H
#include "element.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE;
/* A BACKTICK INDEX (the code element's run record): the last position of
 * a run of each length up to `capacity`, recorded as a scan for a closer
 * passes it, and whether a scan has reached the end of the run, after which
 * a length whose last run lies behind the cursor has no closer. That holds
 * only while the scans that share an index start in the order of the input
 * and past each other's closers, as the code element's own do: a scan that
 * reads ahead of them, from openers the run may not take, keeps an index of
 * its own. Allocated when its first scan looks for a closer. */
typedef struct {
    bufsize_t *positions;
    bufsize_t capacity;
    bool scanned;
} markdown_core_backticks;
/* The same scan on an index of the caller's. */
bufsize_t markdown_core_inline_scan_backticks(markdown_core_backticks *backticks,
                                              markdown_core_inline_state *inline_state, bufsize_t openticklength);
void markdown_core_backticks_free(markdown_core_backticks *backticks);
#endif
