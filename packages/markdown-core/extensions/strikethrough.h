#ifndef MARKDOWN_CORE_STRIKETHROUGH_H
#define MARKDOWN_CORE_STRIKETHROUGH_H

#include "markdown-core-extensions.h"

// Compile-time extension node type; values continue the core inline range
// in bundled-extension order (strikethrough, formula, directive).
#define MARKDOWN_CORE_NODE_STRIKETHROUGH ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000b))

markdown_core_extension *markdown_core_strikethrough_extension(void);

#endif
