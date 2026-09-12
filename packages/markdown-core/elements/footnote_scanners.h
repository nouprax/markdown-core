#ifndef MARKDOWN_CORE_FOOTNOTE_SCANNERS_H
#define MARKDOWN_CORE_FOOTNOTE_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_footnote_definition(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
