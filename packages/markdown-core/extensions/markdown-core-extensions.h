#ifndef MARKDOWN_CORE_CORE_EXTENSIONS_H
#define MARKDOWN_CORE_CORE_EXTENSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core-extension-api.h"
#include "markdown-core-export.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MARKDOWN_CORE_FORMULA_MODE_NONE = 0,
    MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
    MARKDOWN_CORE_FORMULA_MODE_STANDALONE
} markdown_core_formula_mode;

MARKDOWN_CORE_EXPORT
uint16_t markdown_core_extensions_get_table_columns(markdown_core_node *node);

/** Sets the number of columns for the table, returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_table_columns(markdown_core_node *node, uint16_t n_columns);

MARKDOWN_CORE_EXPORT
uint8_t *markdown_core_extensions_get_table_alignments(markdown_core_node *node);

/** Sets the alignments for the table, returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_table_alignments(markdown_core_node *node, uint16_t ncols, uint8_t *alignments);

MARKDOWN_CORE_EXPORT
int markdown_core_extensions_get_table_row_is_header(markdown_core_node *node);

/** Sets whether the node is a table header row, returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_table_row_is_header(markdown_core_node *node, int is_header);

MARKDOWN_CORE_EXPORT
bool markdown_core_extensions_get_tasklist_item_checked(markdown_core_node *node);

/** Sets whether a tasklist item is "checked" (completed), returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_tasklist_item_checked(markdown_core_node *node, bool is_checked);

/** Returns the literal formula payload for formula extension nodes, or NULL on error.
 */
MARKDOWN_CORE_EXPORT
const char *markdown_core_extensions_get_formula_literal(markdown_core_node *node);

/** Sets the literal formula payload for formula extension nodes, returning 1 on success and 0 on
 * error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_formula_literal(markdown_core_node *node, const char *literal);

/** Returns the paragraph-internal layout mode for formula extension nodes.
 */
MARKDOWN_CORE_EXPORT
markdown_core_formula_mode markdown_core_extensions_get_formula_mode(markdown_core_node *node);

/** Sets the paragraph-internal layout mode for formula extension nodes.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_formula_mode(markdown_core_node *node, markdown_core_formula_mode mode);

/** Returns the directive name for directive extension nodes, or NULL on
 * error.
 */
MARKDOWN_CORE_EXPORT
const char *markdown_core_extensions_get_directive_name(markdown_core_node *node);

/** Sets the directive name for directive extension nodes, returning 1
 * on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_directive_name(markdown_core_node *node, const char *name);

/** Returns directive attributes as a normalized JSON object containing only string keys and
 * string values. Returns NULL when the node is not a directive or has no attributes. Markdown
 * attribute-list syntax such as {id=123 muted=true title="My Video"} is represented as
 * {"id":"123","muted":"true","title":"My Video"}. The returned pointer remains valid until
 * the attributes are replaced or the owning node is freed.
 */
MARKDOWN_CORE_EXPORT
const char *markdown_core_extensions_get_directive_attributes(markdown_core_node *node);

/** Sets directive attributes from a JSON object containing only string keys and string values.
 * The object is parsed and normalized. Returns 1 on success and 0 on error; failure leaves the
 * node unchanged.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_set_directive_attributes(markdown_core_node *node, const char *attributes);

#ifdef __cplusplus
}
#endif

#endif
