#include "comment_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */

/*!re2c
  htmlcomment = "--" ([^\x00-]+ | "-" [^\x00-] | "--" [^\x00>])* "-->";
*/

bufsize_t _scan_html_comment(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  htmlcomment { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_end_2(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* '-->' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
