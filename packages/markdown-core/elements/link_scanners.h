#ifndef MARKDOWN_CORE_LINK_SCANNERS_H
#define MARKDOWN_CORE_LINK_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_link_title(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_dangerous_url(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
