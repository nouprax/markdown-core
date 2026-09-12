#ifndef MARKDOWN_CORE_AUTOLINK_SCANNERS_H
#define MARKDOWN_CORE_AUTOLINK_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_scheme(const unsigned char *input, const unsigned char *limit);
#define scan_scheme(c, n) _scan_at(&_scan_scheme, c, n)
bufsize_t _scan_autolink_uri(const unsigned char *input, const unsigned char *limit);
#define scan_autolink_uri(c, n) _scan_at(&_scan_autolink_uri, c, n)
bufsize_t _scan_autolink_email(const unsigned char *input, const unsigned char *limit);
#define scan_autolink_email(c, n) _scan_at(&_scan_autolink_email, c, n)
#ifdef __cplusplus
}
#endif
#endif
