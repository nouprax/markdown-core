#ifndef MARKDOWN_CORE_FORMULA_SCANNERS_H
#define MARKDOWN_CORE_FORMULA_SCANNERS_H
#include "scanners.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t _scan_formula_dollar_inline_open(const unsigned char *input, const unsigned char *limit);
#define scan_formula_dollar_inline_open(c, l, n) _ext_scan_at(&_scan_formula_dollar_inline_open, c, l, n)
bufsize_t _scan_formula_dollar_backtick_open(const unsigned char *input, const unsigned char *limit);
#define scan_formula_dollar_backtick_open(c, l, n) _ext_scan_at(&_scan_formula_dollar_backtick_open, c, l, n)
bufsize_t _scan_formula_dollar_display_open(const unsigned char *input, const unsigned char *limit);
#define scan_formula_dollar_display_open(c, l, n) _ext_scan_at(&_scan_formula_dollar_display_open, c, l, n)
bufsize_t _scan_formula_latex_backslash_inline_open(const unsigned char *input, const unsigned char *limit);
#define scan_formula_latex_backslash_inline_open(c, l, n)                                                              \
    _ext_scan_at(&_scan_formula_latex_backslash_inline_open, c, l, n)
bufsize_t _scan_formula_latex_backslash_display_open(const unsigned char *input, const unsigned char *limit);
#define scan_formula_latex_backslash_display_open(c, l, n)                                                             \
    _ext_scan_at(&_scan_formula_latex_backslash_display_open, c, l, n)
#ifdef __cplusplus
}
#endif
#endif
