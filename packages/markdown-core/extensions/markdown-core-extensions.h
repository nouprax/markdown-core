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

/** The node types the core extensions add, as COMPILE-TIME CONSTANTS.
 *
 * They used to be `markdown_core_node_type` globals assigned by runtime
 * registration in whatever order `core_extensions_registration` happened to
 * call the `create_*` functions --
 * so a node type's numeric identity was a consequence of a call order, in a
 * different file, that nothing checked. Attach order and type numbering are
 * unrelated facts and conflating them is what made the old globals
 * order-dependent (Q16).
 *
 * THE VALUES ARE EXACTLY THE ONES THE OLD REGISTRATION PRODUCED, measured
 * before the change: blocks continue from `MARKDOWN_CORE_NODE_FOOTNOTE`
 * in the order table, table, table, formula, directive; inlines continue from
 * `MARKDOWN_CORE_NODE_CITE` in the order strikethrough, formula,
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

/** THE DIALECT'S ENGINE CONFIGURATION, and the only place it is written down.
 *
 * The parser has one language. Every extension in the attach table of
 * `core-extensions.c` is always attached and every bit of this option word is
 * always set; nothing selects a subset -- not the facade, not the installed
 * CLI, not a test -- so a feature is public from the commit that adds it here,
 * and every fixture, oracle gate, and audit judges the language that ships.
 *
 * Nothing here strips anything: an HTML comment is a `Comment` node, and a
 * consumer that does not want comments drops the nodes.
 */
#define MARKDOWN_CORE_DIALECT_OPTIONS (MARKDOWN_CORE_OPT_FOOTNOTES)

/** Attaches every extension of the dialect, in this library's one order.
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
int markdown_core_core_extensions_attach(markdown_core_parser *parser);

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

#ifdef __cplusplus
}
#endif

#endif
