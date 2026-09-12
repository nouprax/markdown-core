#ifndef MARKDOWN_CORE_CODE_BLOCK_SCANNERS_H
#define MARKDOWN_CORE_CODE_BLOCK_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_open_code_fence(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_close_code_fence(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
