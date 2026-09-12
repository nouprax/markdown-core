// re2c-encoding: utf8
#include "formula_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */
/*!re2c re2c:indent:string = "  "; */

/*!re2c
  formula_dollar_inline_open = [$];
  formula_dollar_backtick_open = [$][`];
  formula_dollar_display_open = [$][$];
  formula_latex_backslash_inline_open = [\\][\\][(];
  formula_latex_backslash_display_open = [\\][\\]"[";
*/

bufsize_t _scan_formula_dollar_inline_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_dollar_backtick_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_backtick_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_dollar_display_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_latex_backslash_inline_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_latex_backslash_display_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}
