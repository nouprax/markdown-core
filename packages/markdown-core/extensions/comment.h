#ifndef MARKDOWN_CORE_COMMENT_H
#define MARKDOWN_CORE_COMMENT_H
#include "inlines.h"
#include "markdown-core-extensions.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const markdown_core_extension MARKDOWN_CORE_EXTENSION_COMMENT;
#ifdef __cplusplus
}
#endif
void markdown_core_block_convert_comment_block(markdown_core_parser *parser, markdown_core_node *b);
markdown_core_node *markdown_core_comment_make_inline(markdown_core_inline_parser *inline_parser, int from, int to,
                                                      markdown_core_chunk literal);
bool markdown_core_comment_scan_html(markdown_core_inline_parser *inline_parser, bufsize_t pos, unsigned *flags,
                                     bufsize_t *length);
markdown_core_node *markdown_core_comment_make_html(markdown_core_inline_parser *inline_parser, bufsize_t pos,
                                                    bufsize_t length);
#endif
