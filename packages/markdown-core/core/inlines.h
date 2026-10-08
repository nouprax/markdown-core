#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "registry.h"
#include "attributes.h"
#include "parser.h"
#include "element.h"

/* Parses the inline content of the field root `parent`, cut from the
 * content of the root its owning token is in. */
bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_member *parent);

/* Parses the content of the inline root `holder` builds, whose nodes are
 * placed in offsets of its content and keep what their decisions read
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.6). `old`, when not NULL,
 * is the root of the previous tree whose content it reads again, its runs
 * measured from `anchor` in the previous source: the parse takes its nodes
 * whole where they are read as they were. */
void markdown_core_parse_root_inlines(markdown_core_parser *parser, markdown_core_member *holder,
                                      const markdown_core_node *old, uint32_t anchor);

/* Parses the field roots `member` builds (node.h, markdown_core_member) that
 * hold inline content, and theirs in turn. Parsing returns whether ordinary
 * raw whitespace occurred, including in nested fields; OOM stays on parser. */
bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_member *member);

/* Release the run records finished runs gave back to `parser`. */
void markdown_core_inline_release_records(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
