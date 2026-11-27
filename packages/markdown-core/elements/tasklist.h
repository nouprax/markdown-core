#ifndef TASKLIST_H
#define TASKLIST_H

#include "markdown-core-element-api.h"
#include "tokenizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE TASK MARKER, code by code: past the spaces after a list item's
 * marker, `[`, one character, `]`, a space or tab and the spaces after
 * it. What it found is the asker's: the block start's found storage (WHAT
 * THE START FOUND, block_internal.h) begins with it -- the offsets on the
 * line of the `[`, and past the spaces, 0 for no marker, and the marker's
 * width. */
typedef struct {
    bufsize_t at, end, width;
} markdown_core_task_found;

extern const markdown_core_construct markdown_core_task_marker;

/* The item takes the marker its start found, of the line `line`. */
void markdown_core_task_take(markdown_core_parser *parser, markdown_core_node *item, markdown_core_reader *reader,
                             const unsigned char *line, const markdown_core_task_found *task);

#ifdef __cplusplus
}
#endif

#endif
