#ifndef MARKDOWN_CORE_CORE_ELEMENTS_H
#define MARKDOWN_CORE_CORE_ELEMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core-element-api.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct markdown_core_revision;

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

/** Parse `source` with the complete core dialect, extended by `setup` when
 *  present (see markdown_core_parser_setup_func), continuing what `revision`
 *  names (parser.h). The product's composition root: the one site that
 *  selects the core dialect for the engine. Tests add instrumentation through
 *  `setup`; no caller selects the language. */
markdown_core_node *markdown_core_parse_revision(const char *source, size_t length,
                                                 markdown_core_parser_setup_func setup, void *context,
                                                 struct markdown_core_revision *revision);

/** A fresh parse through `markdown_core_parse_revision`, in a pool of its
 *  own: the tree it returns holds its slabs. */
markdown_core_node *markdown_core_parse_document_with_setup(const char *source, size_t length,
                                                            markdown_core_parser_setup_func setup, void *context);

/** `markdown_core_parse_document_with_setup` with no setup: the complete
 *  dialect, returning the bare tree for engine tests. The installed API returns
 *  a `markdown_core_document` instead. Release the tree with
 *  `markdown_core_node_free`. */
markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len);

/** Returns the literal formula payload of a formula element node. */
const char *markdown_core_elements_get_formula_literal(markdown_core_node *node);

/** Sets the literal formula payload of a formula element node, returning 0
 * when the copy could not be allocated. */
int markdown_core_elements_set_formula_literal(markdown_core_node *node, const char *literal);

/** Returns the paragraph-internal layout mode of a formula element node. */
markdown_core_formula_mode markdown_core_elements_get_formula_mode(markdown_core_node *node);

/** Sets the paragraph-internal layout mode of a formula element node. */
void markdown_core_elements_set_formula_mode(markdown_core_node *node, markdown_core_formula_mode mode);

/** Returns the directive name, or NULL for a nameless block. */
const char *markdown_core_elements_get_directive_name(markdown_core_node *node);

/** Sets the directive name of a directive element node, returning 0 when
 * the copy could not be allocated. NULL makes the directive nameless. */
int markdown_core_elements_set_directive_name(markdown_core_node *node, const char *name);

#ifdef __cplusplus
}
#endif

#endif
