#ifndef MARKDOWN_CORE_TEXT_SCANNERS_H
#define MARKDOWN_CORE_TEXT_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_spacechars(const unsigned char *input, const unsigned char *limit);
#define scan_spacechars(c, n) _scan_at(&_scan_spacechars, c, n)
bufsize_t _scan_entity(const unsigned char *input, const unsigned char *limit);
#define scan_entity(c, n) _scan_at(&_scan_entity, c, n)
#ifdef __cplusplus
}
#endif
#endif
