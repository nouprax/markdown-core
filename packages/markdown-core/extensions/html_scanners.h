#ifndef MARKDOWN_CORE_HTML_SCANNERS_H
#define MARKDOWN_CORE_HTML_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_html_tag(const unsigned char *input, const unsigned char *limit);
#define scan_html_tag(c, n) _scan_at(&_scan_html_tag, c, n)
bufsize_t _scan_html_pi(const unsigned char *input, const unsigned char *limit);
#define scan_html_pi(c, n) _scan_at(&_scan_html_pi, c, n)
bufsize_t _scan_html_declaration(const unsigned char *input, const unsigned char *limit);
#define scan_html_declaration(c, n) _scan_at(&_scan_html_declaration, c, n)
bufsize_t _scan_html_cdata(const unsigned char *input, const unsigned char *limit);
#define scan_html_cdata(c, n) _scan_at(&_scan_html_cdata, c, n)
bufsize_t _scan_html_block_start(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_start(c, n) _scan_at(&_scan_html_block_start, c, n)
bufsize_t _scan_html_block_start_7(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_start_7(c, n) _scan_at(&_scan_html_block_start_7, c, n)
bufsize_t _scan_html_block_end_1(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_end_1(c, n) _scan_at(&_scan_html_block_end_1, c, n)
bufsize_t _scan_html_block_end_3(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_end_3(c, n) _scan_at(&_scan_html_block_end_3, c, n)
bufsize_t _scan_html_block_end_4(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_end_4(c, n) _scan_at(&_scan_html_block_end_4, c, n)
bufsize_t _scan_html_block_end_5(const unsigned char *input, const unsigned char *limit);
#define scan_html_block_end_5(c, n) _scan_at(&_scan_html_block_end_5, c, n)
#ifdef __cplusplus
}
#endif
#endif
