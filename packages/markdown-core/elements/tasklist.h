#ifndef TASKLIST_H
#define TASKLIST_H

#include "markdown-core-element-api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, immediately after the inherited list algorithm opens an item
 * and finds its first non-space byte, before deciding its first block. */
void markdown_core_parse_task_prefix(markdown_core_parser *parser, markdown_core_node *item,
                                     markdown_core_reader *reader);
/* Where the task prefix at the reader's first non-space byte ends, past the
 * spaces after it, and its marker's width in `*width`; 0 for none. It reads
 * what markdown_core_parse_task_prefix reads, and writes nothing. */
bufsize_t markdown_core_task_prefix_end(markdown_core_reader *reader, bufsize_t *width);

#ifdef __cplusplus
}
#endif

#endif
