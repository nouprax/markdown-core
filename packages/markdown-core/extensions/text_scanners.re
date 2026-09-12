#include "text_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */


bufsize_t _scan_spacechars(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p; \
/*!re2c
  [ \t\v\f\r\n]+ { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_entity(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [&] ([#] ([Xx][A-Fa-f0-9]{1,6}|[0-9]{1,7}) |[A-Za-z][A-Za-z0-9]{1,31} ) [;]
     { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
