#include "text_scanners.h"

/*!include:re2c "scanner_common.re" */


bufsize_t scan_spacechars(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t start = p;
/*!re2c
  [ \t\v\f\r\n]+ { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t scan_entity(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [&] ([#] ([Xx][A-Fa-f0-9]{1,6}|[0-9]{1,7}) |[A-Za-z][A-Za-z0-9]{1,31} ) [;]
     { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
