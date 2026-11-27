#ifndef MARKDOWN_CORE_ELEMENT_LINK_H
#define MARKDOWN_CORE_ELEMENT_LINK_H
#include "inlines.h"
#include "bracket.h"
#include "citation_state.h"
bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length);
int markdown_core_inline_link_label(markdown_core_inline_state *inline_state, markdown_core_chunk *raw_label);
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
/* The space of reference labels (roster.h): a link reference definition's,
 * folded. */
extern const markdown_core_space MARKDOWN_CORE_REFERENCE_SPACE;
/* A link's bracket, opened by a `[` (bracket.h). */
extern const markdown_core_bracket_shape MARKDOWN_CORE_LINK_BRACKET;
/* The destination and the title of a link as values: their references
 * and escapes decoded, by the elements `link` (the link element's instance)
 * holds as peers. */
markdown_core_chunk markdown_core_clean_url(const markdown_core_element_instance *link, markdown_core_chunk *url,
                                            int *lost);
markdown_core_optional_chunk markdown_core_clean_title(const markdown_core_element_instance *link,
                                                       markdown_core_chunk *title, int *lost);

/* The length of the link reference definition at the front of `input`, 0
 * when there is none. */
bufsize_t markdown_core_reference_definition_length(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser, markdown_core_chunk *input);

#endif
