#include "heading_scanners.h"

/*!include:re2c "scanner_common.re" */


bufsize_t scan_atx_heading_start(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [#]{1,6} ([ \t]+|[\r\n])  { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t scan_setext_heading_line(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
/*!re2c
  [=]+ [ \t]* [\r\n] { return 1; }
  [-]+ [ \t]* [\r\n] { return 2; }
  * { return 0; }
*/
}
