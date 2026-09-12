#ifndef MARKDOWN_CORE_FORMULA_SCANNERS_H
#define MARKDOWN_CORE_FORMULA_SCANNERS_H
#include "markdown-core.h"
#ifdef __cplusplus
extern "C" {
#endif
bufsize_t scan_formula_dollar_inline_open(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_formula_dollar_backtick_open(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_formula_dollar_display_open(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_formula_latex_backslash_inline_open(const unsigned char *data, bufsize_t length, bufsize_t offset);
bufsize_t scan_formula_latex_backslash_display_open(const unsigned char *data, bufsize_t length, bufsize_t offset);
#ifdef __cplusplus
}
#endif
#endif
