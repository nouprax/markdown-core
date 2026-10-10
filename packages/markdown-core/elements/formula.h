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

/** A fenced code block whose info string is the word `formula` becomes a
 * standalone FormulaBlock in place as it closes, holding the code's literal;
 * the code block element asks this of each of its blocks once it has read
 * the info string. */
void markdown_core_formula_take_code(const markdown_core_element_instance *formula, markdown_core_parser *parser,
                                     markdown_core_member *member);

#ifdef __cplusplus
}
#endif

#endif
