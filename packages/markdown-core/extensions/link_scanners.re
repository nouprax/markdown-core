#include "link_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */


bufsize_t _scan_link_title(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  ["] (escaped_char|[^"\x00])* ["]   { return (bufsize_t)(p - start); }
  ['] (escaped_char|[^'\x00])* ['] { return (bufsize_t)(p - start); }
  [(] (escaped_char|[^()\x00])* [)]  { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_dangerous_url(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  'data:image/' ('png'|'gif'|'jpeg'|'webp') { return 0; }
  'javascript:' | 'vbscript:' | 'file:' | 'data:' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
