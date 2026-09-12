#ifndef MARKDOWN_CORE_ELEMENT_MEDIA_H
#define MARKDOWN_CORE_ELEMENT_MEDIA_H
#include "inlines.h"
struct bracket;
void markdown_core_inline_apply_image_dimensions(markdown_core_inline_state *inline_state, const struct bracket *opener,
                                                 markdown_core_node *image, bufsize_t end);
void markdown_core_media_record_text(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                     bufsize_t endpos);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_MEDIA;
/* Parse one raw label suffix atomically. Callers record its separator while
 * recognizing their own label grammar; no search or allocation occurs here. */
bool markdown_core_parse_dimensions(markdown_core_chunk label, bufsize_t suffix, bufsize_t separator_length,
                                    markdown_core_dimensions *value, size_t *work);

#endif
