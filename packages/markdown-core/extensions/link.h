#ifndef MARKDOWN_CORE_EXT_LINK_H
#define MARKDOWN_CORE_EXT_LINK_H
#include "inlines.h"
struct bracket;
markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost);
markdown_core_optional_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost);
bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length);
int markdown_core_inline_link_label(markdown_core_inline_parser *subj, markdown_core_chunk *raw_label);
bufsize_t markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input,
                                               markdown_core_map *refmap, markdown_core_attribute_parser *attributes,
                                               uint64_t source_key);
bool markdown_core_block_resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b);
typedef enum { LINK_UNMATCHED, LINK_SHORTCUT, LINK_EXPLICIT } markdown_core_link_match;
typedef struct {
    markdown_core_map_record *record;
    markdown_core_chunk url;
    markdown_core_optional_chunk title;
    bool explicit_tail;
} markdown_core_link_candidate;
markdown_core_link_match markdown_core_link_recognize(markdown_core_inline_parser *subj, struct bracket *opener,
                                                      markdown_core_link_candidate *candidate);
bool markdown_core_link_commit(markdown_core_parser *parser, markdown_core_inline_parser *subj, struct bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos);
#endif
