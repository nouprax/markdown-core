// re2c-encoding: utf8
#include "formula_scanners.h"

/*!include:re2c "scanner_common.re" */
/*!re2c re2c:indent:string = "  "; */

/*!re2c
  formula_dollar_inline_open = [$];
  formula_dollar_backtick_open = [$][`];
  formula_dollar_display_open = [$][$];
  formula_latex_backslash_inline_open = [\\][\\][(];
  formula_latex_backslash_display_open = [\\][\\]"[";
*/

bufsize_t scan_formula_dollar_inline_open(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t start = p;
/*!re2c
    formula_dollar_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_formula_dollar_backtick_open(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t start = p;
/*!re2c
    formula_dollar_backtick_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_formula_dollar_display_open(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t start = p;
/*!re2c
    formula_dollar_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_formula_latex_backslash_inline_open(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_formula_latex_backslash_display_open(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}
