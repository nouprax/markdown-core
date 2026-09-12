#ifndef MARKDOWN_CORE_EXT_LIST_H
#define MARKDOWN_CORE_EXT_LIST_H
#include "inlines.h"
#include "block_internal.h"
void markdown_core_block_finalize_list(markdown_core_node *list);
bool markdown_core_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                 markdown_core_chunk *input, const markdown_core_node *joining, bool *taken);
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_LIST;
#endif
