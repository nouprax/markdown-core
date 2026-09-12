#include "code_block_scanners.h"

/*!include:re2c "scanner_common.re" */


bufsize_t scan_open_code_fence(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [`]{3,} / [^`\r\n\x00]*[\r\n] { return (bufsize_t)(p - start); }
  [~]{3,} / [^\r\n\x00]*[\r\n] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t scan_close_code_fence(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [`]{3,} / [ \t]*[\r\n] { return (bufsize_t)(p - start); }
  [~]{3,} / [ \t]*[\r\n] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
