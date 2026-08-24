#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-extensions.h"

/* C LINKAGE, AND WINDOWS IS THE ONLY PLACE THIS SHOWS. The Itanium ABI does not
 * mangle a variable at global scope, so `MARKDOWN_CORE_EXTENSION_*` resolves on
 * Linux and macOS whether or not the declaration says `extern "C"`; MSVC mangles
 * every variable, and a C++ translation unit including this header without the
 * guard fails to link with LNK2019. */
#ifdef __cplusplus
extern "C" {
#endif

/** The one, immutable descriptor. `core-extensions.c`'s table is the only
 * place its position in the attach order is written down. */
extern const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_DIRECTIVE;

int markdown_core_directive_has_label(markdown_core_node *node);

#ifdef __cplusplus
}
#endif

#endif
