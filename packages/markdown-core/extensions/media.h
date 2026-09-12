#ifndef MARKDOWN_CORE_EXT_MEDIA_H
#define MARKDOWN_CORE_EXT_MEDIA_H
#include "inlines.h"
struct bracket;
void markdown_core_inline_apply_image_dimensions(markdown_core_inline_parser *subj, const struct bracket *opener,
                                                 markdown_core_node *image, bufsize_t end);
void markdown_core_media_record_text(markdown_core_parser *parser, markdown_core_inline_parser *subj, bufsize_t endpos);
#endif
