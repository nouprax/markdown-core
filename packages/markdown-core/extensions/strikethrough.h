#ifndef MARKDOWN_CORE_STRIKETHROUGH_H
#define MARKDOWN_CORE_STRIKETHROUGH_H

#include "markdown-core-extensions.h"

extern markdown_core_node_type MARKDOWN_CORE_NODE_STRIKETHROUGH;
markdown_core_syntax_extension *create_strikethrough_extension(void);

#endif
