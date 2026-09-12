#ifndef MARKDOWN_CORE_SCANNERS_H
#define MARKDOWN_CORE_SCANNERS_H
#include "chunk.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The scanner borrows [offset, length); no sentinel write or input padding. */
bufsize_t _ext_scan_at(bufsize_t (*scanner)(const unsigned char *, const unsigned char *), const unsigned char *input,
                       int length, bufsize_t offset);
bufsize_t _scan_at(bufsize_t (*scanner)(const unsigned char *, const unsigned char *), const markdown_core_chunk *input,
                   bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
