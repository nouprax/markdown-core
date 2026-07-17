#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"

markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost);
markdown_core_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost);

MARKDOWN_CORE_EXPORT
void markdown_core_parse_inlines(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options
);

bufsize_t
markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input, markdown_core_map *refmap);

/* The special-character tables live in the parser (parser-local, never
 * process-global); reset installs the core defaults. */
void markdown_core_inlines_reset_special_chars(markdown_core_parser *parser);
void markdown_core_inlines_add_special_character(markdown_core_parser *parser, unsigned char c, bool emphasis);
void markdown_core_inlines_remove_special_character(markdown_core_parser *parser, unsigned char c, bool emphasis);

#ifdef __cplusplus
}
#endif

#endif
