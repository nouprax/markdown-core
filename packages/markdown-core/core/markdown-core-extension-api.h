#ifndef MARKDOWN_CORE_EXTENSION_API_H
#define MARKDOWN_CORE_EXTENSION_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"

struct markdown_core_chunk;

/**
 * ## Extension Support
 *
 * While the "core" of libmarkdown_core is strictly compliant with the
 * specification, an API is provided for extension writers to
 * hook into the parsing process.
 *
 * It should be noted that the markdown_core_node API already offers
 * room for customization, with methods offered to traverse and
 * modify the AST, and even define extension-specific blocks.
 * When the desired customization is achievable in an error-proof
 * way using that API, it should be the preferred method.
 *
 * The following API requires a more in-depth understanding
 * of libmarkdown_core's parsing strategy, which is exposed
 * [here](http://spec.commonmark.org/0.24/#appendix-a-parsing-strategy).
 *
 * It should be used when "a posteriori" modification of the AST
 * proves to be too difficult / impossible to implement correctly.
 *
 * It can also serve as an intermediary step before extending
 * the specification, as an extension implemented using this API
 * will be trivially integrated in the core if it proves to be
 * desirable.
 */

typedef struct markdown_core_plugin markdown_core_plugin;

/** A syntax extension that can be attached to a markdown_core_parser
 * with markdown_core_parser_attach_syntax_extension().
 *
 * Extension writers should assign functions matching
 * the signature of the following 'virtual methods' to
 * implement new functionality.
 *
 * Their calling order and expected behaviour match the procedure outlined
 * at <http://spec.commonmark.org/0.24/#phase-1-block-structure>:
 *
 * During step 1, markdown_core will call the function provided through
 * its `last_block_matches` hook when it
 * iterates over an open block created by this extension,
 * to determine  whether it could contain the new line.
 * If no function was provided, markdown_core will close the block.
 *
 * During step 2, if and only if the new line doesn't match any
 * of the standard syntax rules, markdown_core will call the function
 * in its `try_opening_block` hook
 * to let the extension determine whether that new line matches
 * one of its syntax rules.
 * It is the responsibility of the parser to create and add the
 * new block with markdown_core_parser_make_block and markdown_core_parser_add_child.
 * If no function was provided is NULL, the extension will have
 * no effect at all on the final block structure of the AST.
 *
 * #### Inline parsing phase hooks
 *
 * For each character in the DISPATCH set the extension declares through
 * its `dispatch` byte set,
 * its `match_inline` hook
 * will get called, it is the responsibility of the extension
 * to scan the characters located at the current inline parsing offset
 * with the markdown_core_inline_parser API.
 *
 * Depending on the type of the extension, it can either:
 *
 * * Scan forward, determine that the syntax matches and return
 *   a newly-created inline node with the appropriate type.
 *   This is the technique that would be used if inline code
 *   (with backticks) was implemented as an extension.
 * * Scan only the character(s) that its syntax rules require
 *   for opening and closing nodes, push a delimiter on the
 *   delimiter stack, and return a simple text node with its
 *   contents set to the character(s) consumed.
 *   This is the technique that would be used if emphasis
 *   inlines were implemented as an extension.
 *
 * When an extension has pushed delimiters on the stack,
 * the function provided through
 * its `insert_inline_from_delim` hook
 * will get called in a latter phase,
 * when the inline parser has matched opener and closer delimiters
 * created by the extension together.
 *
 * It is then the responsibility of the extension to modify
 * and populate the opener inline text node, and to remove
 * the necessary delimiters from the delimiter stack.
 *
 * Finally, the extension should return NULL if its scan didn't
 * match its syntax rules.
 *
 * The extension can store whatever private data it might need
 * with its own private state,
 * and optionally define a free function for this data.
 */
typedef struct subject markdown_core_inline_parser;

/** A delimiter names its RULE, not a byte.
 *
 * It used to carry an `unsigned char delim_char`, and three separate things
 * were derived from that byte:
 *
 *   WHO OWNS IT -- `get_extension_for_special_char` walked the attached
 *   extensions and returned the first whose dispatch set contained the byte.
 *   Two extensions may claim one byte (`autolink` and `directive` both claim
 *   `:`), so the answer was attach order; and if no extension claimed it the
 *   answer was NULL, which `process_emphasis`'s `else if` chain then fell
 *   straight through -- **without advancing the cursor** -- freeing the
 *   delimiter and reading it again on the next turn. Measured: ASan
 *   `heap-use-after-free`, READ of size 1 in `process_emphasis`, from an
 *   extension that pushes a byte it does not itself dispatch; with
 *   `can_open` set it is an infinite loop instead. That is **D33**.
 *
 *   WHICH OPENER MATCHES -- `opener->delim_char == closer->delim_char`, which
 *   is why `formula` needed four distinct sentinel BYTES (0x01-0x04) to keep
 *   `$x$` from matching `$$x$$`, and `directive` a fifth (0x08). Those bytes
 *   are ordinary file bytes: a literal 0x01 in a document split a text run and
 *   was offered to `formula`'s inline hook.
 *
 *   WHERE THE OPENER MEMO LIVES -- `openers_bottom[length % 3][delim_char]`,
 *   an array declared `[3][128]` and indexed by a byte the PUBLIC push
 *   accepts unconstrained. `openers_bottom[2][200]` is offset 456 into 384
 *   elements.
 *
 * A dense rule id answers all three: the owner is on the delimiter, matching is
 * `opener->rule == closer->rule`, and the memo is sized by construction.
 */
typedef enum {
    MARKDOWN_CORE_DELIM_RULE_NONE = 0,
    /* Core. */
    MARKDOWN_CORE_DELIM_RULE_EMPHASIS,     /* `*` */
    MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,   /* `_` */
    MARKDOWN_CORE_DELIM_RULE_SINGLE_QUOTE, /* `'`, smart punctuation only */
    MARKDOWN_CORE_DELIM_RULE_DOUBLE_QUOTE, /* `"`, smart punctuation only */
    /* Extensions. One entry per rule, not per extension and not per byte. */
    MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_INLINE,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_DISPLAY,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_INLINE,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_DISPLAY,
    MARKDOWN_CORE_DELIM_RULE_DIRECTIVE_LABEL,
    MARKDOWN_CORE_DELIM_RULE_COUNT
} markdown_core_delimiter_rule;

/** The delimiter stack's element, OPAQUE.
 *
 * The struct was spelled out here under the comment "Exposed raw for now" from
 * 1.0 until Step 3, which made every field part of the extension surface. The
 * three extensions that push delimiters read eight fields between them and
 * write none; those eight reads are the accessors below and the definition now
 * lives in `core/delimiter.h`.
 */
typedef struct delimiter delimiter;

MARKDOWN_CORE_EXPORT
delimiter *markdown_core_delimiter_previous(const delimiter *delim);

MARKDOWN_CORE_EXPORT
delimiter *markdown_core_delimiter_next(const delimiter *delim);

/** The literal text node the delimiter was pushed for. */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_delimiter_node(const delimiter *delim);

MARKDOWN_CORE_EXPORT
markdown_core_delimiter_rule markdown_core_delimiter_rule_of(const delimiter *delim);

/** The subject offset just past the delimiter's last byte. */
MARKDOWN_CORE_EXPORT
bufsize_t markdown_core_delimiter_position(const delimiter *delim);

/** How many bytes the delimiter run owns. */
MARKDOWN_CORE_EXPORT
bufsize_t markdown_core_delimiter_length(const delimiter *delim);

MARKDOWN_CORE_EXPORT
int markdown_core_delimiter_can_open(const delimiter *delim);

MARKDOWN_CORE_EXPORT
int markdown_core_delimiter_can_close(const delimiter *delim);

/**
 * ### Plugin API.
 *
 * Extensions should be distributed as dynamic libraries,
 * with a single exported function named after the distributed
 * filename.
 *
 * When discovering extensions (see markdown_core_init), markdown_core will
 * try to load a symbol named "init_{{filename}}" in all the
 * dynamic libraries it encounters.
 *
 * For example, given a dynamic library named myextension.so
 * (or myextension.dll), markdown_core will try to load the symbol
 * named "init_myextension". This means that the filename
 * must lend itself to forming a valid C identifier, with
 * the notable exception of dashes, which will be translated
 * to underscores, which means markdown_core will look for a function
 * named "init_my_extension" if it encounters a dynamic library
 * named "my-extension.so".
 *
 * See the 'markdown_core_plugin_init_func' typedef for the exact prototype
 * this function should follow.
 *
 * For now the extensibility of markdown_core is not complete, as
 * it only offers API to hook into the block parsing phase
 * (<http://spec.commonmark.org/0.24/#phase-1-block-structure>).
 *
 * See 'markdown_core_plugin_register_syntax_extension' for more information.
 */

/** The prototype plugins' init function should follow.
 */
typedef int (*markdown_core_plugin_init_func)(markdown_core_plugin *plugin);

/** Register a syntax 'extension' with the 'plugin', it will be made
 * available as an extension and, if attached to a markdown_core_parser
 * with 'markdown_core_parser_attach_syntax_extension', it will contribute
 * to the block parsing process.
 *
 * See the documentation for 'markdown_core_syntax_extension' for information
 * on how to implement one.
 *
 * This function will typically be called from the init function
 * of external modules.
 *
 * This takes ownership of 'extension', one should not call
 * 'markdown_core_syntax_extension_free' on a registered extension.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_plugin_register_syntax_extension(markdown_core_plugin *plugin,
                                                   const markdown_core_syntax_extension *extension);

/** Should create and add a new open block to 'parent_container' if
 * 'input' matches a syntax rule for that block type. It is allowed
 * to modify the type of 'parent_container'.
 *
 * Should return the newly created block if there is one, or
 * 'parent_container' if its type was modified, or NULL.
 */
typedef markdown_core_node *(*markdown_core_open_block_func)(const markdown_core_syntax_extension *extension,
                                                             int indented, markdown_core_parser *parser,
                                                             markdown_core_node *parent_container, unsigned char *input,
                                                             int len);

typedef markdown_core_node *(*markdown_core_match_inline_func)(const markdown_core_syntax_extension *extension,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               unsigned char character,
                                                               markdown_core_inline_parser *inline_parser);

typedef delimiter *(*markdown_core_inline_from_delim_func)(const markdown_core_syntax_extension *extension,
                                                           markdown_core_parser *parser,
                                                           markdown_core_inline_parser *inline_parser,
                                                           delimiter *opener, delimiter *closer);

/** Returned by a 'markdown_core_match_block_func' when 'input' is the
 *  container's own closing line.
 *
 *  The parser closes the container and every block still open inside it, ends
 *  the container at THIS line, and stops processing the line. Returning 1 and
 *  consuming the fence is not enough: the container stays open, and the next
 *  non-blank line is taken as a lazy paragraph continuation and pulled inside
 *  it, on the wrong line.
 *
 *  0 and 1 keep their meanings, so an extension that never returns this is
 *  unaffected.
 */
#define MARKDOWN_CORE_BLOCK_CLOSED 2

/** Should return 'true' if 'input' can be contained in 'container',
 *  'false' otherwise, or MARKDOWN_CORE_BLOCK_CLOSED if 'input' is the
 *  container's own closing line.
 */
typedef int (*markdown_core_match_block_func)(const markdown_core_syntax_extension *extension,
                                              markdown_core_parser *parser, unsigned char *input, int len,
                                              markdown_core_node *container);

typedef const char *(*markdown_core_get_type_string_func)(const markdown_core_syntax_extension *extension,
                                                          markdown_core_node *node);

typedef int (*markdown_core_can_contain_func)(const markdown_core_syntax_extension *extension, markdown_core_node *node,
                                              markdown_core_node_type child);

typedef int (*markdown_core_contains_inlines_func)(const markdown_core_syntax_extension *extension,
                                                   markdown_core_node *node);

typedef int (*markdown_core_accepts_lines_func)(const markdown_core_syntax_extension *extension,
                                                markdown_core_node *node);

typedef markdown_core_node *(*markdown_core_postprocess_func)(const markdown_core_syntax_extension *extension,
                                                              markdown_core_parser *parser, markdown_core_node *root);

typedef int (*markdown_core_ispunct_func)(char c);

typedef void (*markdown_core_opaque_alloc_func)(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                                markdown_core_node *node);

typedef void (*markdown_core_opaque_free_func)(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                               markdown_core_node *node);

/** A syntax extension is a `static const` descriptor in a fixed compile-time
 * table (`extensions/core-extensions.c`), not an object built at run time.
 *
 * There used to be `markdown_core_syntax_extension_new`, sixteen setters, a
 * `free`, and a `priv`/`free_function` pair no extension in this repository
 * ever used. Between them they made the descriptor mutable, heap-allocated
 * from a hidden process-global allocator (`core/syntax_extension.c`'s `_mem`),
 * and reachable only through a process-global registry keyed by NAME
 * (`core/registry.c`) which a process-global once-flag filled in. All of it is
 * gone: every hook takes a `const` descriptor, so "carries no mutable state" is
 * a fact the compiler checks rather than a convention.
 */

/** See the documentation for 'markdown_core_syntax_extension'
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func);

/** Return the index of the line currently being parsed, starting with 1.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_line_number(markdown_core_parser *parser);

/** Return the offset in bytes in the line being processed.
 *
 * Example:
 *
 * ### foo
 *
 * Here, offset will first be 0, then 5 (the index of the 'f' character).
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_offset(markdown_core_parser *parser);

/**
 * Return the offset in 'columns' in the line being processed.
 *
 * This value may differ from the value returned by
 * markdown_core_parser_get_offset() in that it accounts for tabs,
 * and as such should not be used as an index in the current line's
 * buffer.
 *
 * Example:
 *
 * markdown_core_parser_advance_offset() can be called to advance the
 * offset by a number of columns, instead of a number of bytes.
 *
 * In that case, if offset falls "in the middle" of a tab
 * character, 'column' and offset will differ.
 *
 * ```
 * foo                 \t bar
 * ^                   ^^
 * offset (0)          20
 * ```
 *
 * If markdown_core_parser_advance_offset is called here with 'columns'
 * set to 'true' and 'offset' set to 22, markdown_core_parser_get_offset()
 * will return 20, whereas markdown_core_parser_get_column() will return
 * 22.
 *
 * Additionally, as tabs expand to the next multiple of 4 column,
 * markdown_core_parser_has_partially_consumed_tab() will now return
 * 'true'.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_column(markdown_core_parser *parser);

/** Return the absolute index in bytes of the first nonspace
 * character coming after the offset as returned by
 * markdown_core_parser_get_offset() in the line currently being processed.
 *
 * Example:
 *
 * ```
 *   foo        bar            baz  \n
 * ^               ^           ^
 * 0            offset (16) first_nonspace (28)
 * ```
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser);

/** Declare that 'node''s content -- which the caller SET rather than the parser
 * parsing it -- begins at (line, column) in the source, and runs on from there
 * without a break. Returns 1, or 0 if it could not be recorded, in which case
 * the parse is marked lost.
 *
 * A block whose content the parser copied in line by line gets this from
 * `add_line`. A block whose content an extension handed it -- a table cell cut
 * out of a row, a directive's label -- has none, and every position inside it
 * then falls back to arithmetic on the block's own start column, which is right
 * only while the content is one line beginning where the block does. One mark
 * is the whole answer for content that is one line long, which is what all of
 * those are.
 */
/** Copy the marks covering [from, from + length) of 'owner''s content onto
 * 'node', rebased so the first covers 'node''s own offset zero. Returns 1, or 0
 * when there is nothing to copy.
 *
 * For content that is a SLICE of another block's content and more than one line
 * long -- the paragraph a table was split out of -- where one mark would put
 * every line of it on the first line's row.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length);

MARKDOWN_CORE_EXPORT
int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line, int column);

/** Name the source line and BYTE column, both counted from 1, of the byte at
 * 'content_offset' in 'node''s content buffer, and return 1. Returns 0,
 * leaving both outputs untouched, for a node that never took a line.
 *
 * A block's content is the concatenation of the line slices the parser copied
 * into it with the container prefix stripped, so an offset in it is NOT a
 * column: `"> foo\nbar"` strips two bytes from the first line and none from
 * the second, and the two lines of one paragraph's content then start at
 * different source columns. This is the only thing that knows which.
 *
 * The map is live for as long as the parse is: an extension may ask while the
 * block is open, and the inline phase may ask after every block has closed.
 * the parse transaction releases it with the rest of the parse state.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t content_offset,
                                       int *line, int *column);

/** Return the absolute index of the first nonspace column coming after 'offset'
 * in the line currently being processed, counting tabs as multiple
 * columns as appropriate.
 *
 * See the documentation for markdown_core_parser_get_first_nonspace() and
 * markdown_core_parser_get_column() for more information.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser);

/** Return the difference between the values returned by
 * markdown_core_parser_get_first_nonspace_column() and
 * markdown_core_parser_get_column().
 *
 * This is not a byte offset, as it can count one tab as multiple
 * characters.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_indent(markdown_core_parser *parser);

/** Return 'true' if the line currently being processed has been entirely
 * consumed, 'false' otherwise.
 *
 * Example:
 *
 * ```
 *   foo        bar            baz  \n
 * ^
 * offset
 * ```
 *
 * This function will return 'false' here.
 *
 * ```
 *   foo        bar            baz  \n
 *                 ^
 *              offset
 * ```
 * This function will still return 'false'.
 *
 * ```
 *   foo        bar            baz  \n
 *                                ^
 *                             offset
 * ```
 *
 * At this point, this function will now return 'true'.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_is_blank(markdown_core_parser *parser);

/** Return 'true' if the value returned by markdown_core_parser_get_offset()
 * is 'inside' an expanded tab.
 *
 * See the documentation for markdown_core_parser_get_column() for more
 * information.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser);

/** Return the length in bytes of the previously processed line, excluding potential
 * newline (\n) and carriage return (\r) trailing characters.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_get_last_line_length(markdown_core_parser *parser);

/** Add a child to 'parent' during the parsing process.
 *
 * If 'parent' isn't the kind of node that can accept this child,
 * this function will back up till it hits a node that can, closing
 * blocks as appropriate.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column);

/** Advance the 'offset' of the parser in the current line.
 *
 * See the documentation of markdown_core_parser_get_offset() and
 * markdown_core_parser_get_column() for more information.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns);

/** Attach the syntax 'extension' to the 'parser', to provide extra syntax
 *  rules.
 *  See the documentation for markdown_core_syntax_extension for more information.
 *
 *  Returns 'true' if the 'extension' was successfully attached,
 *  'false' otherwise.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_attach_syntax_extension(markdown_core_parser *parser,
                                                 const markdown_core_syntax_extension *extension);

/** Change the type of 'node'.
 *
 * Return 0 if the type could be changed, 1 otherwise.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_type(markdown_core_node *node, markdown_core_node_type type);

/** Return the string content for all types of 'node'.
 *  The pointer stays valid as long as 'node' isn't freed.
 */
MARKDOWN_CORE_EXPORT const char *markdown_core_node_get_string_content(markdown_core_node *node);

/** Set the string 'content' for all types of 'node'.
 *  Copies 'content'.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_string_content(markdown_core_node *node, const char *content);

/** Get the syntax extension responsible for the creation of 'node'.
 *  Return NULL if 'node' was created because it matched standard syntax rules.
 */
MARKDOWN_CORE_EXPORT const markdown_core_syntax_extension *
markdown_core_node_get_syntax_extension(markdown_core_node *node);

/** Set the syntax extension responsible for creating 'node'.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_syntax_extension(markdown_core_node *node,
                                                                 const markdown_core_syntax_extension *extension);

/**
 * ## Inline syntax extension helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

/** Should return 'true' if the predicate matches 'c', 'false' otherwise
 */
typedef int (*markdown_core_inline_predicate)(int c);

/** Advance the current inline parsing offset */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_advance_offset(markdown_core_inline_parser *parser);

/** Get the current inline parsing offset */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_get_offset(markdown_core_inline_parser *parser);

/** Set the offset in bytes in the chunk being processed by the given inline parser.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_set_offset(markdown_core_inline_parser *parser, int offset);

/** Gets the markdown_core_chunk being operated on by the given inline parser.
 * Use markdown_core_inline_parser_get_offset to get our current position in the chunk.
 */
MARKDOWN_CORE_EXPORT
struct markdown_core_chunk *markdown_core_inline_parser_get_chunk(markdown_core_inline_parser *parser);

/** Returns 1 if the inline parser is currently in a bracket; pass 1 for 'image'
 * if you want to know about an image-type bracket, 0 for link-type. */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_in_bracket(markdown_core_inline_parser *parser, int image);

/** Remove the last n characters from the last child of the given node.
 * This only works where all n characters are in the single last child, and the last
 * child is MARKDOWN_CORE_NODE_TEXT.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_node_unput(markdown_core_node *node, int n);

/** Get the character located at the current inline parsing offset
 */
MARKDOWN_CORE_EXPORT
unsigned char markdown_core_inline_parser_peek_char(markdown_core_inline_parser *parser);

/** Get the character located 'pos' bytes in the current line.
 */
MARKDOWN_CORE_EXPORT
unsigned char markdown_core_inline_parser_peek_at(markdown_core_inline_parser *parser, int pos);

/** Whether the inline parser has reached the end of the current line
 */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_is_eof(markdown_core_inline_parser *parser);

/** Get the characters located after the current inline parsing offset
 * while 'pred' matches. Free after usage.
 */
MARKDOWN_CORE_EXPORT
char *markdown_core_inline_parser_take_while(markdown_core_inline_parser *parser, markdown_core_inline_predicate pred);

/** Push a delimiter on the delimiter stack.
 * See <<http://spec.commonmark.org/0.24/#phase-2-inline-structure> for
 * more information on the parameters
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_push_delimiter(markdown_core_inline_parser *parser,
                                                const markdown_core_syntax_extension *owner,
                                                markdown_core_delimiter_rule rule, int can_open, int can_close,
                                                markdown_core_node *inl_text);

/** Remove 'delim' from the delimiter stack
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_remove_delimiter(markdown_core_inline_parser *parser, delimiter *delim);

MARKDOWN_CORE_EXPORT
delimiter *markdown_core_inline_parser_get_last_delimiter(markdown_core_inline_parser *parser);

MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_get_line(markdown_core_inline_parser *parser);

MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_get_column(markdown_core_inline_parser *parser);

/** Make the Text node a delimiter run stands as: its literal is the bytes
 * [from, to] of the block's content and its position is a projection of that
 * range. Returns NULL for a range outside the content.
 *
 * ONE constructor, because there were two hand-written copies of it -- one in
 * `formula`, one in `strikethrough` -- and they disagreed about where the
 * cursor was when they ran, so each computed the run's columns from a different
 * end. Passing the range says it once. The cursor is NOT moved: a caller that
 * has not consumed the run yet still has to.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_inline_parser_make_delimiter_text(markdown_core_inline_parser *parser, int from,
                                                                    int to);

/** Convenience function to scan a given delimiter.
 *
 * 'left_flanking' and 'right_flanking' will be set to true if they
 * respectively precede and follow a non-space, non-punctuation
 * character.
 *
 * Additionally, 'punct_before' and 'punct_after' will respectively be set
 * if the preceding or following character is a punctuation character.
 *
 * Note that 'left_flanking' and 'right_flanking' can both be 'true'.
 *
 * Returns the number of delimiters encountered, in the limit
 * of 'max_delims', and advances the inline parsing offset.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_scan_delimiters(markdown_core_inline_parser *parser, int max_delims, unsigned char c,
                                                int *left_flanking, int *right_flanking, int *punct_before,
                                                int *punct_after);

MARKDOWN_CORE_EXPORT
void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add);

MARKDOWN_CORE_EXPORT
markdown_core_llist *markdown_core_parser_get_syntax_extensions(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
