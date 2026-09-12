/*!re2c re2c:indent:string = '  '; */

#include <stdlib.h>
#include "ext_scanners.h"

/* Scanners borrow an exact slice. A virtual NUL at its limit replaces the
 * former write-and-restore sentinel; neither input padding nor writable bytes
 * belong to the scanner contract. Cursor/marker offsets can consume a virtual
 * terminator without forming a pointer outside the borrowed slice. */
bufsize_t _ext_scan_at(bufsize_t (*scanner)(const unsigned char *, const unsigned char *),
                        const unsigned char *ptr, int len, bufsize_t offset)
{
    if (!ptr || offset < 0 || offset >= len) return 0;
    return scanner(ptr + offset, ptr + len);
}

/*!re2c
  re2c:define:YYCTYPE  = "unsigned char";
  re2c:define:YYCURSOR = p;
  re2c:define:YYMARKER = marker;
  re2c:api = custom;
  re2c:api:style = free-form;
  re2c:define:YYPEEK = "(p < length ? input[p] : 0)";
  re2c:define:YYSKIP = "++p;";
  re2c:define:YYSHIFT = "p += @@{shift};";
  re2c:define:YYBACKUP = "marker = p;";
  re2c:define:YYRESTORE = "p = marker;";
  re2c:define:YYBACKUPCTX = "marker = p;";
  re2c:define:YYRESTORECTX = "p = marker;";
  re2c:yyfill:enable = 0;

  spacechar = [ \t\v\f];
  newline = [\r][\n]? | [\n];
  escaped_char = [\\][|!"#$%&'()*+,./:;<=>?@[\\\]^_`{}~-];

  table_marker = (spacechar*[:]?[-]+[:]?spacechar*);
  table_cell = (escaped_char|[^|\r\n\000])+;

  formula_dollar_inline_open = [$];
  formula_dollar_backtick_open = [$][`];
  formula_dollar_display_open = [$][$];
  formula_latex_backslash_inline_open = [\\][\\][(];
  formula_latex_backslash_display_open = [\\][\\]"[";

*/

bufsize_t _scan_table_start(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    [|]? table_marker ([|] table_marker)* [|]? spacechar* newline {
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
    [|] spacechar* { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_table_row_end(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    spacechar* newline { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_dollar_inline_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_dollar_backtick_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_backtick_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_dollar_display_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t start = p;
/*!re2c
    formula_dollar_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_latex_backslash_inline_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_inline_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

bufsize_t _scan_formula_latex_backslash_display_open(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
    formula_latex_backslash_display_open { return (bufsize_t)(p - start); }
    * { return 0; }
  */
}

/* Iterate lexical dash runs in an exact line-body slice. Whitespace is not
 * geometry: the caller maps authored tabs to source columns. */
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
