#include "code_block_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */


bufsize_t _scan_open_code_fence(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [`]{3,} / [^`\r\n\x00]*[\r\n] { return (bufsize_t)(p - start); }
  [~]{3,} / [^\r\n\x00]*[\r\n] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_close_code_fence(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [`]{3,} / [ \t]*[\r\n] { return (bufsize_t)(p - start); }
  [~]{3,} / [ \t]*[\r\n] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
