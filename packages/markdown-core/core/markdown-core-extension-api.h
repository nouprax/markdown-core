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
 * with markdown_core_parser_attach_extension().
 *
 * An extension is an immutable compile-time descriptor whose fields are
 * functions matching the signature of the following 'virtual methods'.
 *
 * Their calling order and expected behaviour match the procedure outlined
 * at <http://spec.commonmark.org/0.24/#phase-1-block-structure>:
 *
 * During step 1, markdown_core will call the descriptor's
 * 'last_block_matches' function when it
 * iterates over an open block created by this extension,
 * to determine  whether it could contain the new line.
 * If no function was provided, markdown_core will close the block.
 *
 * During step 2, if and only if the new line doesn't match any
 * of the standard syntax rules, markdown_core will call the descriptor's
 * 'try_opening_block' function
 * to let the extension determine whether that new line matches
 * one of its syntax rules.
 * It is the responsibility of the parser to create and add the
 * new block with markdown_core_parser_make_block and markdown_core_parser_add_child.
 * If no function was provided is NULL, the extension will have
 * no effect at all on the final block structure of the AST.
 *
 * #### Inline parsing phase hooks
 *
 * For each character listed in the descriptor's
 * 'special_inline_chars' array,
 * the descriptor's
 * 'match_inline' function
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
 * the descriptor's
 * 'insert_inline_from_delim' function
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
 */
typedef struct subject markdown_core_inline_parser;

/** Exposed raw for now */

typedef struct delimiter {
    struct delimiter *previous;
    struct delimiter *next;
    markdown_core_node *inl_text;
    bufsize_t position;
    bufsize_t length;
    unsigned char delim_char;
    int can_open;
    int can_close;
} delimiter;

/** This will search for the syntax extension named 'name' among the
 *  bundled syntax extensions (immutable compile-time descriptors; there is
 *  no runtime registration).
 *
 *  It can then be attached to a markdown_core_parser
 *  with the markdown_core_parser_attach_extension method.
 */
MARKDOWN_CORE_EXPORT
markdown_core_extension *markdown_core_extension_find(const char *name);

/** Returns a caller-owned list of the bundled syntax extensions.
 */
MARKDOWN_CORE_EXPORT
markdown_core_llist *markdown_core_extension_list(markdown_core_mem *mem);

/** Should create and add a new open block to 'parent_container' if
 * 'input' matches a syntax rule for that block type. It is allowed
 * to modify the type of 'parent_container'.
 *
 * Should return the newly created block if there is one, or
 * 'parent_container' if its type was modified, or NULL.
 */
typedef markdown_core_node *(*markdown_core_open_block_func)(
    markdown_core_extension *extension,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
);

typedef markdown_core_node *(*markdown_core_match_inline_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
);

typedef delimiter *(*markdown_core_inline_from_delim_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
);

/** Should return 'true' if 'input' can be contained in 'container',
 *  'false' otherwise.
 */
typedef int (*markdown_core_match_block_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *container
);

typedef const char *(*markdown_core_get_type_string_func)(markdown_core_extension *extension, markdown_core_node *node);

typedef int (*markdown_core_can_contain_func)(
    markdown_core_extension *extension,
    markdown_core_node *node,
    markdown_core_node_type child
);

typedef int (*markdown_core_contains_inlines_func)(markdown_core_extension *extension, markdown_core_node *node);

typedef int (*markdown_core_accepts_lines_func)(markdown_core_extension *extension, markdown_core_node *node);

/** Block-local postprocess hook. After inline parsing, footnote processing,
 * and per-block text consolidation, the parser calls this once for every
 * block (and every inline-owning node, such as a table cell or directive
 * label wrapper) in document order. All effects must stay inside that
 * node's subtree so the pipeline can later rerun for single blocks.
 *
 * Returns the node now occupying the block's position in the tree: the
 * block itself, or its replacement when the extension replaced or retyped
 * the block. Must not return NULL. */
typedef markdown_core_node *(*markdown_core_postprocess_block_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *block
);

typedef int (*markdown_core_ispunct_func)(char c);

typedef void (*markdown_core_alloc_opaque_func)(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
);

typedef void (*markdown_core_free_opaque_func)(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *node
);

/** See the documentation for 'markdown_core_extension'
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
markdown_core_node *markdown_core_parser_add_child(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_node_type block_type,
    int start_column
);

/** Advance the 'offset' of the parser in the current line.
 *
 * See the documentation of markdown_core_parser_get_offset() and
 * markdown_core_parser_get_column() for more information.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns);

MARKDOWN_CORE_EXPORT
void markdown_core_parser_feed_reentrant(markdown_core_parser *parser, const char *buffer, size_t len);

/** Attach the syntax 'extension' to the 'parser', to provide extra syntax
 *  rules.
 *  See the documentation for markdown_core_extension for more information.
 *
 *  Returns 'true' if the 'extension' was successfully attached,
 *  'false' otherwise.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_parser_attach_extension(markdown_core_parser *parser, markdown_core_extension *extension);

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
MARKDOWN_CORE_EXPORT markdown_core_extension *markdown_core_node_get_extension(markdown_core_node *node);

/** Set the syntax extension responsible for creating 'node'.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_extension(markdown_core_node *node, markdown_core_extension *extension);

/**
 * ## Inline syntax extension helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

/** Should return 'true' if the predicate matches 'c', 'false' otherwise
 */
typedef int (*markdown_core_inline_predicate_func)(int c);

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
char *
markdown_core_inline_parser_take_while(markdown_core_inline_parser *parser, markdown_core_inline_predicate_func pred);

/** Push a delimiter on the delimiter stack.
 * See <<http://spec.commonmark.org/0.24/#phase-2-inline-structure> for
 * more information on the parameters
 */
MARKDOWN_CORE_EXPORT
void markdown_core_inline_parser_push_delimiter(
    markdown_core_inline_parser *parser,
    unsigned char c,
    int can_open,
    int can_close,
    markdown_core_node *inl_text
);

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
void markdown_core_parser_manage_extensions_special_characters(markdown_core_parser *parser, int add);

MARKDOWN_CORE_EXPORT
markdown_core_llist *markdown_core_parser_get_extensions(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
