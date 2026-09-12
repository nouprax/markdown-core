#ifndef MARKDOWN_CORE_AUTOLINK_SCANNERS_H
#define MARKDOWN_CORE_AUTOLINK_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_scheme(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_autolink_uri(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_autolink_email(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
