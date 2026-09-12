#include "html_scanners.h"

/*!include:re2c "scanner_config.re" */
/*!include:re2c "text_grammar.re" */

/*!re2c
  tagname = [A-Za-z][A-Za-z0-9-]*;
  blocktagname = 'address'|'article'|'aside'|'base'|'basefont'|'blockquote'|'body'|'caption'|'center'|'col'|'colgroup'|'dd'|'details'|'dialog'|'dir'|'div'|'dl'|'dt'|'fieldset'|'figcaption'|'figure'|'footer'|'form'|'frame'|'frameset'|'h1'|'h2'|'h3'|'h4'|'h5'|'h6'|'head'|'header'|'hr'|'html'|'iframe'|'legend'|'li'|'link'|'main'|'menu'|'menuitem'|'nav'|'noframes'|'ol'|'optgroup'|'option'|'p'|'param'|'search'|'section'|'title'|'summary'|'table'|'tbody'|'td'|'tfoot'|'th'|'thead'|'title'|'tr'|'track'|'ul';
  attributename = [a-zA-Z_:][a-zA-Z0-9:._-]*;
  unquotedvalue = [^ \t\r\n\v\f"'=<>`\x00]+;
  singlequotedvalue = ['][^'\x00]*['];
  doublequotedvalue = ["][^"\x00]*["];
  attributevalue = unquotedvalue | singlequotedvalue | doublequotedvalue;
  attributevaluespec = spacechar* [=] spacechar* attributevalue;
  attribute = spacechar+ attributename attributevaluespec?;
  opentag = tagname attribute* spacechar* [/]? [>];
  closetag = [/] tagname spacechar* [>];
  processinginstruction = ([^?>\x00]+ | [?][^>\x00] | [>])+;
  declaration = [A-Za-z]+ [^>\x00]*;
  cdata = "CDATA[" ([^\]\x00]+ | "]" [^\]\x00] | "]]" [^>\x00])*;
  htmltag = opentag | closetag;
*/

bufsize_t _scan_html_tag(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  htmltag { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_pi(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  processinginstruction { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_declaration(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
  (void) marker;
/*!re2c
  declaration { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_cdata(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  cdata { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_start(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
/*!re2c
  [<] ('script'|'pre'|'textarea'|'style') (spacechar | [>]) { return 1; }
  '<!--' { return 2; }
  '<?' { return 3; }
  '<!' [A-Za-z] { return 4; }
  '<![CDATA[' { return 5; }
  [<] [/]? blocktagname (spacechar | [/]? [>])  { return 6; }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_start_7(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
/*!re2c
  [<] (opentag | closetag) [\t\n\f ]* [\r\n] { return 7; }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_end_1(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* [<] [/] ('script'|'pre'|'textarea'|'style') [>] { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_end_3(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* '?>' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_end_4(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* '>' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}

bufsize_t _scan_html_block_end_5(const unsigned char *input, const unsigned char *limit)
{
  size_t p = 0, length = (size_t)(limit - input);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [^\n\x00]* ']]>' { return (bufsize_t)(p - start); }
  * { return 0; }
*/
}
