#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-extensions.h"

/** The one, immutable descriptor. `core-extensions.c`'s table is the only
 * place its position in the attach order is written down. */
extern const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_DIRECTIVE;

int markdown_core_directive_has_label(markdown_core_node *node);

#endif
