#ifndef MARKDOWN_CORE_ELEMENT_LINK_H
#define MARKDOWN_CORE_ELEMENT_LINK_H
#include "inlines.h"
#include "bracket_state.h"
bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length);
int markdown_core_inline_link_label(markdown_core_inline_state *inline_state, markdown_core_chunk *raw_label);
/* Reads the link reference definitions at the front of `b`'s content into
 * Reference nodes, put in `b`'s parent before `b` in source order, and drops
 * their bytes from the content. Whether any content is left that is not
 * blank. */
bool markdown_core_block_resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b);
typedef enum { LINK_UNMATCHED, LINK_SHORTCUT, LINK_EXPLICIT } markdown_core_link_match;
typedef struct {
    /* The normalized label a reference names; empty for a direct link. */
    markdown_core_chunk label;
    markdown_core_chunk url;
    markdown_core_optional_chunk title;
    bool explicit_tail;
} markdown_core_link_candidate;
markdown_core_link_match markdown_core_link_recognize(const markdown_core_element_instance *link,
                                                      markdown_core_inline_state *inline_state, struct bracket *opener,
                                                      markdown_core_link_candidate *candidate);
bool markdown_core_link_commit(const markdown_core_element_instance *link, markdown_core_parser *parser,
                               markdown_core_inline_state *inline_state, struct bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_LINK;
markdown_core_chunk markdown_core_clean_url(markdown_core_chunk *url, int *lost);
markdown_core_optional_chunk markdown_core_clean_title(markdown_core_chunk *title, int *lost);

/* The length of the link reference definition at the front of `input`, 0
 * when there is none. */
bufsize_t markdown_core_reference_definition_length(markdown_core_chunk *input,
                                                    markdown_core_attribute_parser *attributes);

void markdown_core_inline_pop_bracket(const markdown_core_element_instance *link,
                                      markdown_core_inline_state *inline_state);
markdown_core_node *markdown_core_inline_handle_close_bracket(const markdown_core_element_instance *link,
                                                              markdown_core_parser *parser,
                                                              markdown_core_inline_state *inline_state);
void markdown_core_inline_take_bracket_content(const markdown_core_element_instance *link, markdown_core_parser *parser,
                                               bracket *opener, markdown_core_node *owner);
void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement);
void markdown_core_inline_push_bracket(const markdown_core_element_instance *link,
                                       markdown_core_inline_state *inline_state, bracket_kind kind,
                                       markdown_core_node *inl_text);

#endif
