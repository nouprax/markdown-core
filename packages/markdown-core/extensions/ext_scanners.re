/*!re2c re2c:indent:string = '  '; */

#include <stdlib.h>
#include "ext_scanners.h"

markdown_core_bufsize markdown_core_ext_scan_at(markdown_core_bufsize (*scanner)(const unsigned char *), unsigned char *ptr, int len, markdown_core_bufsize offset)
{
	markdown_core_bufsize res;

        if (ptr == NULL || offset >= len) {
          return 0;
        } else {
	  unsigned char lim = ptr[len];

	  ptr[len] = '\0';
	  res = scanner(ptr + offset);
	  ptr[len] = lim;
        }

	return res;
}

/*!re2c
  re2c:define:YYCTYPE  = "unsigned char";
  re2c:define:YYCURSOR = p;
  re2c:define:YYMARKER = marker;
  re2c:yyfill:enable = 0;

  spacechar = [ \t\v\f];
  newline = [\r]?[\n];
  escaped_char = [\\][|!"#$%&'()*+,./:;<=>?@[\\\]^_`{}~-];

  table_marker = (spacechar*[:]?[-]+[:]?spacechar*);
  table_cell = (escaped_char|[^|\r\n\000])+;

  tasklist = ("[ ]"|"[x]"|"[X]")spacechar+;

  formula_dollar_inline_open = [$];
  formula_dollar_display_open = [$][$];
  formula_latex_backslash_inline_open = [\\][\\][(];
  formula_latex_backslash_display_open = [\\][\\]"[";

  directive_name_char = [A-Za-z0-9_-];
  directive_name = directive_name_char+;
*/

markdown_core_bufsize markdown_core_scan_table_start(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    [|]? table_marker ([|] table_marker)* [|]? spacechar* newline {
      return (markdown_core_bufsize)(p - start);
    }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_table_cell(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    // In fact, `table_cell` matches non-empty table cells only. The empty
    // string is also a valid table cell, but is handled by the default rule.
    // This approach prevents re2c's match-empty-string warning.
    table_cell { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_table_cell_end(const unsigned char *p)
{
  const unsigned char *start = p;
  /*!re2c
    [|] spacechar* { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_table_row_end(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    spacechar* newline { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_tasklist(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    tasklist { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_formula_dollar_inline_open(const unsigned char *p)
{
  const unsigned char *start = p;
  /*!re2c
    formula_dollar_inline_open { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_formula_dollar_display_open(const unsigned char *p)
{
  const unsigned char *start = p;
  /*!re2c
    formula_dollar_display_open { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_formula_latex_backslash_inline_open(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    formula_latex_backslash_inline_open { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_formula_latex_backslash_display_open(const unsigned char *p)
{
  const unsigned char *marker = NULL;
  const unsigned char *start = p;
  /*!re2c
    formula_latex_backslash_display_open { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}

markdown_core_bufsize markdown_core_scan_directive_name(const unsigned char *p)
{
  const unsigned char *start = p;
  /*!re2c
    directive_name { return (markdown_core_bufsize)(p - start); }
    * { return 0; }
  */
}
