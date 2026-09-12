#include "comment_scanners.h"

/*!include:re2c "scanner_common.re" */

/*!re2c
  htmlcomment = "--" ([^\x00-]+ | "-" [^\x00-] | "--" [^\x00>])* "-->";
*/

bufsize_t scan_html_comment(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  htmlcomment { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t scan_html_block_end_2(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* '-->' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
