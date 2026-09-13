#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"
#include "attributes.h"
#include "parser.h"
#include "element.h"

MARKDOWN_CORE_EXPORT
bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap);

/* Shared field ownership and inline parsing. Parsing returns whether ordinary
 * raw whitespace occurred, including in nested fields; OOM stays on parser. */
bool markdown_core_parse_inline_subtrees(markdown_core_parser *parser, markdown_core_node *node,
                                         markdown_core_map *refmap);

/* Parser-local projections are built entirely from the attached syntax declarations. */
void markdown_core_inlines_reset_special_chars(markdown_core_parser *parser);
/* Project the lifecycle hook implementers of the current registry; false on
 * allocation failure, leaving the previous projection in place. */
bool markdown_core_inlines_project_hooks(markdown_core_parser *parser);
/* The projection of a registry that is not (yet) the parser's, so a registry
 * and its projection can be published together or not at all. */
bool markdown_core_inlines_project_hooks_of(markdown_core_mem *mem, const markdown_core_element *const *elements,
                                            size_t count, markdown_core_inline_hooks *projected);
void markdown_core_inlines_release_hooks(markdown_core_parser *parser);
void markdown_core_inlines_add_text_terminator(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_remove_text_terminator(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_add_flanking_transparent(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_remove_flanking_transparent(markdown_core_parser *parser, unsigned char c);

#ifdef __cplusplus
}
#endif

#endif
