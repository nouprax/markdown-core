#ifndef MARKDOWN_CORE_ELEMENT_LINK_H
#define MARKDOWN_CORE_ELEMENT_LINK_H
#include "inlines.h"
#include "bracket_state.h"
bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length);
int markdown_core_inline_link_label(markdown_core_inline_state *inline_state, markdown_core_chunk *raw_label);
bool markdown_core_block_resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b);
typedef enum { LINK_UNMATCHED, LINK_SHORTCUT, LINK_EXPLICIT } markdown_core_link_match;
typedef struct {
    markdown_core_map_record *record;
    markdown_core_chunk url;
    markdown_core_optional_chunk title;
    bool explicit_tail;
} markdown_core_link_candidate;
markdown_core_link_match markdown_core_link_recognize(markdown_core_inline_state *inline_state, struct bracket *opener,
                                                      markdown_core_link_candidate *candidate);
bool markdown_core_link_commit(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                               struct bracket *opener, markdown_core_link_candidate *candidate, bufsize_t initial_pos);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_LINK;
markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost);
markdown_core_optional_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost);

/* Reads ONE link reference definition off the front of `input`, registers its
 * label and the resource it states in `refmap`, and returns the number of
 * bytes it consumed -- 0 if the front of `input` is not a definition. The
 * definition produces no node (M2): it is consumed, and every reference that
 * resolves to it is the `Link` or `Embedded` it names. A NULL refmap performs the
 * same recognition without registering or allocating a definition resource. */
bufsize_t markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input,
                                               markdown_core_map *refmap, markdown_core_attribute_parser *attributes,
                                               uint64_t source_key);

void markdown_core_inline_pop_bracket(markdown_core_inline_state *inline_state);
markdown_core_node *markdown_core_inline_handle_close_bracket(markdown_core_parser *parser,
                                                              markdown_core_inline_state *inline_state);
void markdown_core_inline_take_bracket_content(markdown_core_parser *parser, bracket *opener,
                                               markdown_core_node *owner);
void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement);
void markdown_core_inline_push_bracket(markdown_core_inline_state *inline_state, bracket_kind kind,
                                       markdown_core_node *inl_text);

#endif
