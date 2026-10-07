#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "registry.h"
#include "attributes.h"
#include "parser.h"
#include "element.h"

/* Parses the inline content of `parent`: an inline root's, whose nodes are
 * placed in offsets of its content, or, when `root` is false, a field's, cut
 * from the content of the root its owning token is in. */
bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_member *parent, bool root);

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
