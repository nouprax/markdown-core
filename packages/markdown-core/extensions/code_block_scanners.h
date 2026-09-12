#ifndef MARKDOWN_CORE_CODE_BLOCK_SCANNERS_H
#define MARKDOWN_CORE_CODE_BLOCK_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_open_code_fence(const unsigned char *input, const unsigned char *limit);
#define scan_open_code_fence(c, n) _scan_at(&_scan_open_code_fence, c, n)
bufsize_t _scan_close_code_fence(const unsigned char *input, const unsigned char *limit);
#define scan_close_code_fence(c, n) _scan_at(&_scan_close_code_fence, c, n)
#ifdef __cplusplus
}
#endif
#endif
