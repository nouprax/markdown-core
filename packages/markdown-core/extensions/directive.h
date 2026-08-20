#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-extensions.h"

extern markdown_core_node_type MARKDOWN_CORE_NODE_DIRECTIVE;
extern markdown_core_node_type MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK;
extern markdown_core_node_type MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;

markdown_core_syntax_extension *create_directive_extension(void);

int markdown_core_directive_has_label(markdown_core_node *node);

#endif
