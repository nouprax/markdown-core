#ifndef MARKDOWN_CORE_TABLE_SCANNERS_H
#define MARKDOWN_CORE_TABLE_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_table_start(const unsigned char *input, const unsigned char *limit);
#define scan_table_start(c, l, n) _ext_scan_at(&_scan_table_start, c, l, n)
bufsize_t _scan_table_cell(const unsigned char *input, const unsigned char *limit);
#define scan_table_cell(c, l, n) _ext_scan_at(&_scan_table_cell, c, l, n)
bufsize_t _scan_table_cell_end(const unsigned char *input, const unsigned char *limit);
#define scan_table_cell_end(c, l, n) _ext_scan_at(&_scan_table_cell_end, c, l, n)
bufsize_t _scan_table_row_end(const unsigned char *input, const unsigned char *limit);
#define scan_table_row_end(c, l, n) _ext_scan_at(&_scan_table_row_end, c, l, n)
int _scan_table_dash(const unsigned char **cursor, const unsigned char *limit, const unsigned char **from);
int _scan_table_horizontal(const unsigned char *input, const unsigned char *limit);
#ifdef __cplusplus
}
#endif
#endif
