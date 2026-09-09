#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"
#include "attributes.h"

/* Parse one raw label suffix atomically. Callers record its separator while
 * recognizing their own label grammar; no search or allocation occurs here. */
bool markdown_core_parse_dimensions(markdown_core_chunk label, bufsize_t suffix, bufsize_t separator_length,
                                    markdown_core_dimensions *value, size_t *work);

int markdown_core_inline_parser_attributes(markdown_core_inline_parser *parser, bufsize_t start,
                                           markdown_core_attributes *value, bufsize_t *end);

markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost);
markdown_core_optional_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost);

MARKDOWN_CORE_EXPORT
void markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap);

/* Reads ONE link reference definition off the front of `input`, registers its
 * label and the resource it states in `refmap`, and returns the number of
 * bytes it consumed -- 0 if the front of `input` is not a definition. The
 * definition produces no node (M2): it is consumed, and every reference that
 * resolves to it is the `Link` or `Media` it names. */
bufsize_t markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input,
                                               markdown_core_map *refmap, markdown_core_attribute_parser *attributes);

/* The special-character tables live in the parser (parser-local, never
 * process-global); reset installs the core defaults. */
void markdown_core_inlines_reset_special_chars(markdown_core_parser *parser);
/* One function per table, because they answer different questions. Both refuse
 * a byte the core already owns: `is_core_special_character` is what keeps an
 * extension from making `\` or `]` mean something else, and it guarded both
 * tables when they were written together. */
void markdown_core_inlines_add_text_terminator(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_remove_text_terminator(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_add_flanking_transparent(markdown_core_parser *parser, unsigned char c);
void markdown_core_inlines_remove_flanking_transparent(markdown_core_parser *parser, unsigned char c);

#ifdef __cplusplus
}
#endif

#endif
