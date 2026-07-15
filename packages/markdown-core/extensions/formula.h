#ifndef MARKDOWN_CORE_FORMULA_H
#define MARKDOWN_CORE_FORMULA_H

#include "markdown-core-extensions.h"

extern markdown_core_node_type MARKDOWN_CORE_NODE_FORMULA;
extern markdown_core_node_type MARKDOWN_CORE_NODE_FORMULA_BLOCK;

markdown_core_syntax_extension *create_formula_extension(void);

#endif
