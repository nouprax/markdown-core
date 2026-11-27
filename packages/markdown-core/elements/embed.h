#ifndef MARKDOWN_CORE_ELEMENT_EMBED_H
#define MARKDOWN_CORE_ELEMENT_EMBED_H
#include "inlines.h"
#include "bracket.h"
/* The image grammar's calls, with `embed` the embed element's
 * instance: the link commit applies an image's dimensions, and the text
 * scanner records each text slice inside an image's label. */
void markdown_core_inline_apply_image_dimensions(const markdown_core_element_instance *embed,
                                                 markdown_core_inline_state *inline_state, const struct bracket *opener,
                                                 markdown_core_node *image, bufsize_t end);
void markdown_core_embed_record_text(const markdown_core_element_instance *embed,
                                     markdown_core_inline_state *inline_state, bufsize_t endpos);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMBED;
/* An image's bracket, opened by a `![`, whose `!` stays as text when the
 * image replaces it (bracket.h). */
extern const markdown_core_bracket_shape MARKDOWN_CORE_EMBED_BRACKET;
/* Parse one raw label suffix atomically. Callers record its separator while
 * recognizing their own label grammar; no search or allocation occurs here. */
bool markdown_core_parse_dimensions(markdown_core_chunk label, bufsize_t suffix, bufsize_t separator_length,
                                    markdown_core_dimensions *value, size_t *work);

/* THE DIMENSION GRAMMAR'S WORK (the embed element's parse record):
 * ordinary image-label bytes and bounded dimension work. A cross-link embed
 * shares the grammar and counts its work in its own record. */
typedef struct {
    size_t dimensions;
} markdown_core_embed_work;

#endif
