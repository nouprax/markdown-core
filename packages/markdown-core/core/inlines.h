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

/** Parses `parent`'s inline content starting at byte `start` of the content
 * buffer, appending to whatever children are already attached. `start` must
 * sit at a line start, and the caller must guarantee nothing in [0, start)
 * can pair with or reshape anything at or after `start` (no special
 * characters before the seam). Position bookkeeping matches a full parse:
 * the subject reads the true buffer, so lookbacks across the seam see the
 * real bytes. */
/** Longest line-aligned, inline-inert common prefix of two content buffers
 * (see the definition for the exact guarantee); 0 when no usable seam
 * exists. */
bufsize_t markdown_core_inline_seam_prefix(
    const struct markdown_core_parser *parser,
    const unsigned char *a,
    bufsize_t a_len,
    const unsigned char *b,
    bufsize_t b_len,
    int options
);

void markdown_core_parse_inlines_from(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options,
    bufsize_t start
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
