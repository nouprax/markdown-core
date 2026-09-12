// re2c-encoding: utf8
#include "table_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */
/*!re2c re2c:indent:string = "  "; */

/*!re2c
  table_marker = (horizontal_space*[:]?[-]+[:]?horizontal_space*);
  table_cell = (escaped_char|[^|\r\n\000])+;
*/

bufsize_t _scan_table_start(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    [|]? table_marker ([|] table_marker)* [|]? horizontal_space* newline {
      return (bufsize_t)(p - start);
    }
    * { return 0; }
  */
}

bufsize_t _scan_table_cell(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
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

bufsize_t _scan_table_cell_end(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    [|] horizontal_space* { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_table_row_end(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    horizontal_space* newline { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

int _scan_table_dash(const unsigned char **cursor, const unsigned char *limit,
                     const unsigned char **from)
{
    const unsigned char *input = *cursor;
    size_t p = 0, length = (size_t)(limit - input);
    for (;;) {
        size_t start = p;
/*!re2c
            [ \t]+ { continue; }
            [-]+ { *from = input + start; *cursor = input + p; return 1; }
            [\x00] { *cursor = input + (p < length ? p : length); return start == length ? 0 : -1; }
            * { *cursor = input + p; return -1; }
        */
    }
}

int _scan_table_horizontal(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
    size_t marker = 0;
/*!re2c
        "+" ":"? "+"* "-" [-+]* ":"? "+" / [\x00] { return p == length ? '-' : 0; }
        "+" ":"? "+"* "=" [=+]* ":"? "+" / [\x00] { return p == length ? '=' : 0; }
        * { return 0; }
    */
}
