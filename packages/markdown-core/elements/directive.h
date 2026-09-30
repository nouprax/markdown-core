#ifndef MARKDOWN_CORE_DIRECTIVE_H
#define MARKDOWN_CORE_DIRECTIVE_H

#include "markdown-core-elements.h"
#include <chunk.h>
#include <node.h>

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
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_DIRECTIVE;

/* A directive node's value, its `opaque`. */
typedef struct {
    markdown_core_chunk name;
    markdown_core_node *label;
    int fence_length;
    int consume_line;
} markdown_core_directive_value;

int markdown_core_directive_has_label(markdown_core_node *node);

/* A directive's label, or NULL: the tree walks read it for every directive. */
static inline markdown_core_node *markdown_core_directive_label(const markdown_core_node *node) {
    const markdown_core_directive_value *directive =
        node && (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE || node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)
            ? (const markdown_core_directive_value *)node->opaque
            : NULL;
    return directive ? directive->label : NULL;
}

#ifdef __cplusplus
}
#endif

#endif
