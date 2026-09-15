#ifndef MARKDOWN_CORE_TEXT_SCANNERS_H
#define MARKDOWN_CORE_TEXT_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_spacechars(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_entity(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
