#ifndef MARKDOWN_CORE_LINK_SCANNERS_H
#define MARKDOWN_CORE_LINK_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_link_title(const unsigned char *input, const unsigned char *limit);
#define scan_link_title(c, n) _scan_at(&_scan_link_title, c, n)
bufsize_t _scan_dangerous_url(const unsigned char *input, const unsigned char *limit);
#define scan_dangerous_url(c, n) _scan_at(&_scan_dangerous_url, c, n)
#ifdef __cplusplus
}
#endif
#endif
