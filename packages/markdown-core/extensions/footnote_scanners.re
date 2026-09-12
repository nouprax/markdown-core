#include "footnote_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */


bufsize_t _scan_footnote_definition(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  '[^' ([^\] \r\n\x00\t]+) ']:' [ \t]* { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
