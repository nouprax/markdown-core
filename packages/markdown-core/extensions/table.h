#ifndef MARKDOWN_CORE_TABLE_H
#define MARKDOWN_CORE_TABLE_H

#include "markdown-core-extensions.h"

extern markdown_core_node_type MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_TABLE_ROW, MARKDOWN_CORE_NODE_TABLE_CELL;

markdown_core_syntax_extension *create_table_extension(void);

#endif
