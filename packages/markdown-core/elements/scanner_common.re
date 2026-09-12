/* Shared byte-scanner configuration and lexical primitives. */
/*!re2c
  re2c:define:YYCTYPE  = "unsigned char";
  re2c:define:YYCURSOR = p;
  re2c:define:YYMARKER = marker;
  re2c:define:YYCTXMARKER = marker;
  re2c:api = custom;
  re2c:api:style = free-form;
  re2c:define:YYPEEK = "(p < remaining ? input[p] : 0)";
  re2c:define:YYSKIP = "++p;";
  re2c:define:YYSHIFT = "p += @@{shift};";
  re2c:define:YYBACKUP = "marker = p;";
  re2c:define:YYRESTORE = "p = marker;";
  re2c:define:YYBACKUPCTX = "marker = p;";
  re2c:define:YYRESTORECTX = "p = marker;";
  re2c:yyfill:enable = 0;

*/

/*!re2c
  spacechar = [ \t\v\f\r\n];
  escaped_char = [\\][!"#$%&'()*+,./:;<=>?@[\\\]^_`{|}~-];
  horizontal_space = [ \t\v\f];
  newline = [\r][\n]? | [\n];
*/
