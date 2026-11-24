/* Shared byte-scanner configuration and lexical primitives. */
/*!re2c
  // A scanner reads `remaining` bytes, and 0 past them, which ends every
  // rule: the classes leave \x00 out. A NUL of the text is a byte like any
  // other control byte, so the scanner reads it as one, \x01, and only the
  // end of the bytes reads as 0.
  re2c:define:YYCTYPE  = "unsigned char";
  re2c:define:YYCURSOR = p;
  re2c:define:YYMARKER = marker;
  re2c:define:YYCTXMARKER = marker;
  re2c:api = custom;
  re2c:api:style = free-form;
  re2c:define:YYPEEK = "(p < remaining ? (input[p] ? input[p] : 1) : 0)";
  re2c:define:YYSKIP = "++p;";
  re2c:define:YYSHIFT = "p += @@{shift};";
  re2c:define:YYBACKUP = "marker = p;";
  re2c:define:YYRESTORE = "p = marker;";
  re2c:define:YYBACKUPCTX = "marker = p;";
  re2c:define:YYRESTORECTX = "p = marker;";
  re2c:yyfill:enable = 0;

*/

/*!re2c
  escaped_char = [\\][!"#$%&'()*+,./:;<=>?@[\\\]^_`{|}~-];
  space_or_tab = [ \t];
  newline = [\r][\n]? | [\n];
  // Spaces, tabs and up to one line ending (docs/specs/dialect.md), and the
  // same with at least one of them.
  spnl = space_or_tab* (newline space_or_tab*)?;
  spnl1 = space_or_tab+ (newline space_or_tab*)? | newline space_or_tab*;
*/
