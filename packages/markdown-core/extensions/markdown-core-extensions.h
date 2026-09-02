#ifndef MARKDOWN_CORE_CORE_EXTENSIONS_H
#define MARKDOWN_CORE_CORE_EXTENSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core-extension-api.h"
#include "markdown-core-export.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/** The node types the six core extensions add, as COMPILE-TIME CONSTANTS.
 *
 * They used to be `markdown_core_node_type` globals assigned by
 * `markdown_core_syntax_extension_add_node` in whatever order
 * `core_extensions_registration` happened to call the `create_*` functions --
 * so a node type's numeric identity was a consequence of a call order, in a
 * different file, that nothing checked. Attach order and type numbering are
 * unrelated facts and conflating them is what made the old globals
 * order-dependent (Q16).
 *
 * THE VALUES ARE EXACTLY THE ONES THE OLD REGISTRATION PRODUCED, measured
 * before the change: blocks continue from `MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION`
 * in the order table, table, table, formula, directive; inlines continue from
 * `MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE` in the order strikethrough, formula,
 * directive, directive. Nothing outside the library can see a value -- the
 * export map is 32 facade functions and `local: *` -- but keeping them makes
 * this a structural change and nothing else.
 *
 * THEY ARE `markdown_core_node_type`, NOT AN ANONYMOUS ENUM OF THEIR OWN, and
 * that is not a style choice. An anonymous enum is a DISTINCT type from
 * `markdown_core_node_type`, so `markdown_core_node_get_type(n) ==
 * MARKDOWN_CORE_NODE_TABLE` compares two different enumeration types and
 * `markdown_core_node_set_type(n, MARKDOWN_CORE_NODE_STRIKETHROUGH)` converts
 * between them. GCC rejects both under `-Wenum-compare` and
 * `-Wenum-conversion`, which `-Wall` turns on; clang says nothing about either
 * unless `-Wanon-enum-enum-conversion` is asked for by name, which no warning
 * group implies. A macro that casts at the one place the value is written
 * makes every one of the ninety-odd use sites exactly typed, and cannot drift.
 */
#define MARKDOWN_CORE_NODE_TABLE ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000b))
#define MARKDOWN_CORE_NODE_TABLE_ROW ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000c))
#define MARKDOWN_CORE_NODE_TABLE_CELL ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000d))
#define MARKDOWN_CORE_NODE_FORMULA_BLOCK ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000e))
#define MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000f))

#define MARKDOWN_CORE_NODE_STRIKETHROUGH ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000b))
#define MARKDOWN_CORE_NODE_FORMULA ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000c))
#define MARKDOWN_CORE_NODE_DIRECTIVE ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000d))
#define MARKDOWN_CORE_NODE_DIRECTIVE_LABEL ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000e))

typedef enum {
    MARKDOWN_CORE_FORMULA_MODE_NONE = 0,
    MARKDOWN_CORE_FORMULA_MODE_EMBEDDED,
    MARKDOWN_CORE_FORMULA_MODE_STANDALONE
} markdown_core_formula_mode;

/** One bit per core extension.  A caller says WHICH extensions it wants and
 * cannot say in what order, because the order is not in the bit values -- it
 * is the order of the table in `core-extensions.c`, and that table is the only
 * place it is written down.
 */
typedef enum {
    MARKDOWN_CORE_CORE_EXTENSION_TABLE = 1u << 0,
    MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH = 1u << 1,
    MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK = 1u << 2,
    MARKDOWN_CORE_CORE_EXTENSION_TASKLIST = 1u << 3,
    MARKDOWN_CORE_CORE_EXTENSION_FORMULA = 1u << 4,
    MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE = 1u << 5
} markdown_core_core_extension_bit;

/** Attaches every core extension named in `mask`, in this library's one order.
 * Returns 1 when all of them attached and 0 when any did not; on failure the
 * parser keeps whatever attached before the failure and the caller is expected
 * to discard it.
 *
 * DELIBERATELY NOT `MARKDOWN_CORE_EXPORT`.  Both product entry points -- the
 * CLI and the facade every binding goes through -- are linked against the
 * static archives, so neither needs the symbol in `core/exports/markdown_core.map`,
 * and putting it there would make the attach order part of the public ABI at
 * the exact moment the point is that callers cannot choose it.
 */
int markdown_core_core_extensions_attach(markdown_core_parser *parser, unsigned mask);

/** The bit for a core extension's registered name, or 0 when the name is not
 * one of them.  This is what routes a `-e NAME` lever back through the ordered
 * table instead of around it.
 */
unsigned markdown_core_core_extensions_bit(const char *name);

/** The name at `index` in the fixed table, or NULL past the end. The CLI's
 * `--help` used to walk a process-global registry list to print this. */
MARKDOWN_CORE_EXPORT
const char *markdown_core_core_extensions_name_at(size_t index);

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

/** Whether the source wrote an attribute container at all. `:n` has none and
 * `:n{}` has an empty one; a count of zero cannot tell them apart.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_directive_has_attributes(markdown_core_node *node);

/** How many attributes the directive carries. Names are unique: `class` values
 * accumulate into one and every other repeat keeps its last value.
 */
MARKDOWN_CORE_EXPORT
size_t markdown_core_extensions_directive_attribute_count(markdown_core_node *node);

/** Reads the attribute at `index`, in first-occurrence source order. A repeated
 * non-class name replaces the value in its original slot; repeated `class`
 * values accumulate there. Returns 1 on success and 0 when the node is not a
 * directive or the index is out of range. The bytes are BORROWED from the node
 * and are not NUL-terminated, which is why each comes with its length.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_extensions_directive_attribute_at(markdown_core_node *node, size_t index, const char **name,
                                                    size_t *name_length, const char **value, size_t *value_length);

#ifdef __cplusplus
}
#endif

#endif
