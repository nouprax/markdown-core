#include "autolink_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */

/*!re2c
  scheme = [A-Za-z][A-Za-z0-9.+-]{1,31};
*/

bufsize_t _scan_scheme(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  scheme [:] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_autolink_uri(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  scheme [:][^\x00-\x20<>]*[>]  { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_autolink_email(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+
    [@]
    [a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?
    ([.][a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*
    [>] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
