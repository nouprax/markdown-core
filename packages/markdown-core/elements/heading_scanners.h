#ifndef MARKDOWN_CORE_HEADING_SCANNERS_H
#define MARKDOWN_CORE_HEADING_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_atx_heading_start(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_setext_heading_line(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
