#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-extensions.h"

markdown_core_syntax_extension *create_directive_extension(void);

int markdown_core_directive_has_label(markdown_core_node *node);

#endif
