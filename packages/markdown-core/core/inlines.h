#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"
#include "attributes.h"
#include "parser.h"
#include "element.h"

bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap);

/* Shared field ownership and inline parsing. Parsing returns whether ordinary
 * raw whitespace occurred, including in nested fields; OOM stays on parser. */
bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_node *node,
                                         markdown_core_map *refmap);

/* Release the run records finished runs gave back to `parser`. */
void markdown_core_inline_release_records(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
