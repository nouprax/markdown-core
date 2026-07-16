#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-extensions.h"

// Compile-time extension node types; see table.h/strikethrough.h for the
// value-range convention.
#define MARKDOWN_CORE_NODE_DIRECTIVE ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000d))
#define MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000f))
#define MARKDOWN_CORE_NODE_DIRECTIVE_LABEL ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000e))

markdown_core_extension *markdown_core_directive_extension(void);

int markdown_core_directive_has_label(markdown_core_node *node);

#endif
