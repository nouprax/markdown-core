#ifndef MARKDOWN_CORE_TABLE_H
#define MARKDOWN_CORE_TABLE_H

#include "markdown-core-extensions.h"

// Compile-time extension node types; values continue the core block range
// in bundled-extension order (table, formula, directive).
#define MARKDOWN_CORE_NODE_TABLE ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000b))
#define MARKDOWN_CORE_NODE_TABLE_ROW ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000c))
#define MARKDOWN_CORE_NODE_TABLE_CELL ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000d))

markdown_core_extension *markdown_core_table_extension(void);

#endif
