#ifndef MARKDOWN_CORE_FOOTNOTE_SCANNERS_H
#define MARKDOWN_CORE_FOOTNOTE_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_footnote_definition(const unsigned char *input, const unsigned char *limit);
#define scan_footnote_definition(c, n) _scan_at(&_scan_footnote_definition, c, n)
#ifdef __cplusplus
}
#endif
#endif
