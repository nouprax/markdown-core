#ifndef MARKDOWN_CORE_TABLE_SCANNERS_H
#define MARKDOWN_CORE_TABLE_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_table_start(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_table_cell(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_table_cell_end(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_table_row_end(const unsigned char *data, bufsize_t length, bufsize_t offset);
int scan_table_dash(const unsigned char **cursor, const unsigned char *limit, const unsigned char **from);
int scan_table_horizontal(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
