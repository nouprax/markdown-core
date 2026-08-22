#ifndef MARKDOWN_CORE_STRIKETHROUGH_H
#define MARKDOWN_CORE_STRIKETHROUGH_H

#include "markdown-core-extensions.h"

/** The one, immutable descriptor. `core-extensions.c`'s table is the only
 * place its position in the attach order is written down. */
extern const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_STRIKETHROUGH;

#endif
