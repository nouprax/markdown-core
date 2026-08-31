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

/** Should create and add a new open block to 'parent_container' if
 * 'input' matches a syntax rule for that block type. It is allowed
 * to modify the type of 'parent_container'.
 *
 * Should return the newly created block if there is one, or
 * 'parent_container' if its type was modified, or NULL.
 */
typedef markdown_core_node *(*markdown_core_open_block_func)(
    const markdown_core_syntax_extension *extension,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
);

typedef markdown_core_node *(*markdown_core_match_inline_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
);

typedef delimiter *(*markdown_core_inline_from_delim_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
);

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
typedef int (*markdown_core_match_block_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *container
);

typedef const char *(*markdown_core_get_type_string_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_node *node
);

typedef int (*markdown_core_can_contain_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child
);

typedef int (*markdown_core_contains_inlines_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_node *node
);

typedef int (*markdown_core_accepts_lines_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_node *node
);

/** THE PER-BLOCK POSTPROCESS (docs/STREAMING.md T18, F15). Called once per
 * projection for every block the descriptor's `postprocess_blocks` selects,
 * after that block's inlines are parsed and consolidated, in extension attach
 * order; the comment strip runs after the last hook. `*block` is IN/OUT:
 * leave it to keep the node, reseat it to the node that replaced it, set it
 * NULL to say the node is gone. An out-parameter has one spelling per
 * outcome where a return value cannot tell "removed" from "unchanged"
 * without a sentinel -- and the whole-tree hook this replaces returned its
 * root unconditionally after an arm may have freed it, safe only because the
 * root was always the DOCUMENT and matched nothing. A hook acts on the block
 * it is handed and inside it; it never touches another block, which is what
 * lets the core act on a queue of blocks after the walk that found them. */
typedef void (*markdown_core_postprocess_block_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block
);

/** Called once, from `finalize`, on a block of this extension's own type, after
 * its scope is settled and before anything reads its content.
 *
 * It exists because a container that the END OF THE INPUT closed and one a
 * fence closed are the same node: unlike a fenced code block, whose `closed`
 * is a field every projection carries, an extension block's close state lives
 * in its opaque payload and reaches no view. This is the only moment at which
 * the extension can still say so, and saying so is a diagnostic, not a rewrite
 * -- a close hook that changed the TREE would be a second `postprocess` with
 * a worse name. Settling the block's OWN payload is different: the core
 * detaches code and html literals from content at this same moment in
 * `finalize`, and a close hook may do the same for a literal that lives in
 * its opaque state (#153: formula freezes its content here and the literal
 * slices it). */
typedef void (*markdown_core_close_block_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *node
);

typedef void (*markdown_core_opaque_alloc_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
);

typedef void (*markdown_core_opaque_free_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
);

/** Copy everything this extension keeps in `node.as` -- the opaque payload,
 * or a plain union arm like a table cell's index -- from `src` onto `dst`,
 * which is a fresh zeroed node of the same type. The AST is derived from the
 * CST by cloning the block skeleton (§12.5), and the core cannot copy what it
 * cannot name: an extension that stores block state and does not say how to
 * copy it fails the derivation rather than losing the state silently.
 * Returns 0 on allocation loss. */
typedef int (*markdown_core_opaque_copy_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *dst,
    const markdown_core_node *src
);

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

/** Hand every concrete record that names 'from' to 'to', keeping each one's
 * role.
 *
 * An extension that REPLACES a block -- a fenced code block whose info line
 * says the body is a formula becomes a formula block -- leaves records naming
 * a node it is about to free. This is how it says the bytes moved with the
 * construct. A record must never name a freed node: that is a map owning a
 * node (D11) one indirection further out, and AddressSanitizer over
 * `--concrete` is what finds it.
 */
/** Declare that 'node''s content -- which the caller SET rather than the parser
 * feeding it -- begins at (line, column) in the source, and runs on from there
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
int markdown_core_parser_adopt_content_marks(
    markdown_core_parser *parser,
    markdown_core_node *owner,
    markdown_core_node *node,
    bufsize_t from,
    bufsize_t length
);

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
 * markdown_core_parser_finish releases it with the rest of the parse state.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_content_place(
    markdown_core_parser *parser,
    markdown_core_node *node,
    bufsize_t content_offset,
    int *line,
    int *column
);

/** Return the indent of the line being processed, in columns: the width
 * between the current offset and the first nonspace character, counting a
 * tab as the columns it expands to.
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

/** Add a child to 'parent' during the parsing process.
 *
 * If 'parent' isn't the kind of node that can accept this child,
 * this function will back up till it hits a node that can, closing
 * blocks as appropriate.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_parser_add_child(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_node_type block_type,
    int start_column
);

/** Advance the 'offset' of the parser in the current line.
 *
 * With 'columns' set, 'count' is measured in columns rather than bytes,
 * and tabs expand to the next multiple of 4 columns.
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
int markdown_core_parser_attach_syntax_extension(
    markdown_core_parser *parser,
    const markdown_core_syntax_extension *extension
);

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

/** Set the syntax extension responsible for creating 'node'.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_syntax_extension(
    markdown_core_node *node,
    const markdown_core_syntax_extension *extension
);

/**
 * ## Inline syntax extension helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

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

/** Push a delimiter on the delimiter stack.
 * See <<http://spec.commonmark.org/0.24/#phase-2-inline-structure> for
 * more information on the parameters
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_push_delimiter(
    markdown_core_inline_parser *parser,
    const markdown_core_syntax_extension *owner,
    markdown_core_delimiter_rule rule,
    int can_open,
    int can_close,
    markdown_core_node *inl_text
);

/** Remove 'delim' from the delimiter stack
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_remove_delimiter(markdown_core_inline_parser *parser, delimiter *delim);

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
markdown_core_node *markdown_core_inline_parser_make_delimiter_text(
    markdown_core_inline_parser *parser,
    int from,
    int to
);

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
int markdown_core_inline_parser_scan_delimiters(
    markdown_core_inline_parser *parser,
    int max_delims,
    unsigned char c,
    int *left_flanking,
    int *right_flanking,
    int *punct_before,
    int *punct_after
);

MARKDOWN_CORE_EXPORT
void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add);

#ifdef __cplusplus
}
#endif

#endif
