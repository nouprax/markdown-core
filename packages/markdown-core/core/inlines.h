#ifndef MARKDOWN_CORE_INLINES_H
#define MARKDOWN_CORE_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "references.h"

struct markdown_core_inline_config;

struct markdown_core_inline_config *markdown_core_inlines_new_config(markdown_core_mem *mem);

markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost);
markdown_core_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost);

MARKDOWN_CORE_EXPORT
void markdown_core_parse_inlines(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options
);

/** Longest line-aligned, inline-inert common prefix of two content buffers
 * (see the definition for the exact guarantee); 0 when no usable seam
 * exists. */
markdown_core_bufsize markdown_core_inline_seam_prefix(
    const struct markdown_core_parser *parser,
    const unsigned char *a,
    markdown_core_bufsize a_len,
    const unsigned char *b,
    markdown_core_bufsize b_len,
    int options
);

/** Parses `parent`'s inline content starting at byte `start` of the content
 * buffer, appending to whatever children are already attached. `start` must
 * sit at a line start, and the caller must guarantee nothing in [0, start)
 * can pair with or reshape anything at or after `start` (no special
 * characters before the seam). Position bookkeeping matches a full parse:
 * the subject reads the true buffer, so lookbacks across the seam see the
 * real bytes. */
void markdown_core_parse_inlines_from(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options,
    markdown_core_bufsize start
);

/** Where a parsed reference definition's spellings sit in the input chunk,
 * so the caller can capture them as concrete records: [0, label_end)
 * spells `[label]:`, [url_start, url_end) the destination exactly as
 * written (angle brackets included), and [title_start, title_end) the
 * title with its delimiters — both zero when the definition carries none,
 * including when a trailing title candidate was rewound back into the
 * paragraph. Meaningful only when the parse returns nonzero. */
typedef struct markdown_core_reference_spans {
    markdown_core_bufsize label_end;
    markdown_core_bufsize url_start;
    markdown_core_bufsize url_end;
    markdown_core_bufsize title_start;
    markdown_core_bufsize title_end;
} markdown_core_reference_spans;

markdown_core_bufsize markdown_core_parse_reference_inline(
    markdown_core_mem *mem,
    markdown_core_chunk *input,
    markdown_core_map *refmap,
    markdown_core_reference_spans *spans
);

#ifdef __cplusplus
}
#endif

#endif
