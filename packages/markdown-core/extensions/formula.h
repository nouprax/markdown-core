#ifndef MARKDOWN_CORE_FORMULA_H
#define MARKDOWN_CORE_FORMULA_H

#include "markdown-core-extensions.h"

// Compile-time extension node types; see table.h/strikethrough.h for the
// value-range convention.
#define MARKDOWN_CORE_NODE_FORMULA ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000c))
#define MARKDOWN_CORE_NODE_FORMULA_BLOCK ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000e))

markdown_core_extension *markdown_core_formula_extension(void);

#endif
