#ifndef TASKLIST_H
#define TASKLIST_H

#include "markdown-core.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, immediately after the inherited list algorithm opens an item
 * and finds its first non-space byte, before deciding its first block. */
void markdown_core_parse_task_prefix(markdown_core_parser *parser, markdown_core_node *item, const unsigned char *input,
                                     bufsize_t len);

#ifdef __cplusplus
}
#endif

#endif
