#ifndef MARKDOWN_CORE_HTML_SCANNERS_H
#define MARKDOWN_CORE_HTML_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_html_tag(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_pi(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_declaration(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_cdata(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_start(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_start_7(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_end_1(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_end_3(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_end_4(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_html_block_end_5(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
