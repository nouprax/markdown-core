#ifndef MARKDOWN_CORE_CORE_ELEMENTS_H
#define MARKDOWN_CORE_CORE_ELEMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core-element-api.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MARKDOWN_CORE_FORMULA_MODE_NONE = 0,
    MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
    MARKDOWN_CORE_FORMULA_MODE_STANDALONE
} markdown_core_formula_mode;

/** Attaches every element of the dialect, in this library's one order.
 * Returns 1 when all of them attached and 0 when any did not; on failure the
 * parser keeps whatever attached before the failure and the caller is expected
 * to discard it.
 *
 * Not in `core/exports/markdown_core.map`: putting it there would make the
 * attach order part of the public ABI, and the point is that callers cannot
 * choose it.
 */
const markdown_core_element *const *markdown_core_core_elements(size_t *count);

/** Returns the literal formula payload for formula element nodes, or NULL on error.
 */
const char *markdown_core_elements_get_formula_literal(markdown_core_node *node);

/** Sets the literal formula payload for formula element nodes, returning 1 on success and 0 on
 * error.
 */
int markdown_core_elements_set_formula_literal(markdown_core_node *node, const char *literal);

/** Returns the paragraph-internal layout mode for formula element nodes.
 */
markdown_core_formula_mode markdown_core_elements_get_formula_mode(markdown_core_node *node);

/** Sets the paragraph-internal layout mode for formula element nodes.
 */
int markdown_core_elements_set_formula_mode(markdown_core_node *node, markdown_core_formula_mode mode);

/** Returns the directive name, NULL for a nameless block, or NULL on
 * error.
 */
const char *markdown_core_elements_get_directive_name(markdown_core_node *node);

/** Sets the directive name for directive element nodes, returning 1
 * on success and 0 on error. NULL selects a nameless DirectiveBlock; an inline
 * directive requires a valid nonempty name.
 */
int markdown_core_elements_set_directive_name(markdown_core_node *node, const char *name);

#ifdef __cplusplus
}
#endif

#endif
