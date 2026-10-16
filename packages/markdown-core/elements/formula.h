#ifndef MARKDOWN_CORE_FORMULA_H
#define MARKDOWN_CORE_FORMULA_H

#include "markdown-core-elements.h"

/* C LINKAGE, AND WINDOWS IS THE ONLY PLACE THIS SHOWS. The Itanium ABI does not
 * mangle a variable at global scope, so `MARKDOWN_CORE_ELEMENT_*` resolves on
 * Linux and macOS whether or not the declaration says `extern "C"`; MSVC mangles
 * every variable, and a C++ translation unit including this header without the
 * guard fails to link with LNK2019. */
#ifdef __cplusplus
extern "C" {
#endif

/** The one, immutable descriptor. `core-elements.c`'s table is the only
 * place its position in the attach order is written down. */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_FORMULA;

/** `node`, a closed fenced code block inside `parent`, becomes a FormulaBlock
 * of `formula`, the dialect's formula instance, when its info string is the
 * word `formula` and `parent` can hold one. It is rewritten in place: it keeps
 * its place, its range and its content, and holds nothing of the code block
 * but the trimmed literal -- no attributes and no info. An allocation failure
 * fails the parse and leaves the code block as it was. */
void markdown_core_formula_promote_fence(const markdown_core_element_instance *formula, markdown_core_parser *parser,
                                         markdown_core_node *node, markdown_core_node *parent);

#ifdef __cplusplus
}
#endif

#endif
