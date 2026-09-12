#ifndef MARKDOWN_CORE_COMMENT_SCANNERS_H
#define MARKDOWN_CORE_COMMENT_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_html_comment(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_end_2(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
