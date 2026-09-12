#include "heading_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */


bufsize_t _scan_atx_heading_start(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [#]{1,6} ([ \t]+|[\r\n])  { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_setext_heading_line(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
/*!re2c
  [=]+ [ \t]* [\r\n] { return 1; }
  [-]+ [ \t]* [\r\n] { return 2; }
  * { return 0; }
*/
}
