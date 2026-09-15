// re2c-encoding: utf8
#include "table_scanners.h"

/*!include:re2c "scanner_common.re" */
/*!re2c re2c:indent:string = "  "; */

/*!re2c
  table_marker = (horizontal_space*[:]?[-]+[:]?horizontal_space*);
  table_cell = (escaped_char|[^|\r\n\000])+;
*/

bufsize_t scan_table_start(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    [|]? table_marker ([|] table_marker)* [|]? horizontal_space* newline {
      return (bufsize_t)(p - start);
    }
    * { return 0; }
  */
}

bufsize_t scan_table_cell(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    // In fact, `table_cell` matches non-empty table cells only. The empty
    // string is also a valid table cell, but is handled by the default rule.
    // This approach prevents re2c's match-empty-string warning.
    table_cell { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_table_cell_end(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t start = p;
/*!re2c
    [|] horizontal_space* { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t scan_table_row_end(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    horizontal_space* newline { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

int scan_table_dash(const unsigned char **cursor, const unsigned char *limit,
                     const unsigned char **from)
{
    const unsigned char *input = *cursor;
    size_t p = 0, remaining = (size_t)(limit - input);
    for (;;) {
        size_t start = p;
/*!re2c
            [ \t]+ { continue; }
            [-]+ { *from = input + start; *cursor = input + p; return 1; }
            [\x00] { *cursor = input + (p < remaining ? p : remaining); return start == remaining ? 0 : -1; }
            * { *cursor = input + p; return -1; }
        */
    }
}

int scan_table_horizontal(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
    size_t marker = 0;
/*!re2c
        "+" ":"? "+"* "-" [-+]* ":"? "+" / [\x00] { return p == remaining ? '-' : 0; }
        "+" ":"? "+"* "=" [=+]* ":"? "+" / [\x00] { return p == remaining ? '=' : 0; }
        * { return 0; }
    */
}
