#ifndef MARKDOWN_CORE_COMMENT_SCANNERS_H
#define MARKDOWN_CORE_COMMENT_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_html_comment(const unsigned char *input, const unsigned char *limit);
#define scan_html_comment(c, n) _scan_at(&_scan_html_comment, c, n)
bufsize_t _scan_html_block_end_2(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_end_2(c, n) _scan_at(&_scan_html_block_end_2, c, n)
#ifdef __cplusplus
}
#endif
#endif
