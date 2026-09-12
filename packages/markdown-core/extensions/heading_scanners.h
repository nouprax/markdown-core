#ifndef MARKDOWN_CORE_HEADING_SCANNERS_H
#define MARKDOWN_CORE_HEADING_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_atx_heading_start(const unsigned char *input, const unsigned char *limit);
#define scan_atx_heading_start(c, n) _scan_at(&_scan_atx_heading_start, c, n)
bufsize_t _scan_setext_heading_line(const unsigned char *input, const unsigned char *limit);
#define scan_setext_heading_line(c, n) _scan_at(&_scan_setext_heading_line, c, n)
#ifdef __cplusplus
}
#endif
#endif
