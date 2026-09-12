#include "footnote_scanners.h"

/*!include:re2c "scanner_common.re" */


bufsize_t scan_footnote_definition(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  '[^' ([^\] \r\n\x00\t]+) ']:' [ \t]* { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
