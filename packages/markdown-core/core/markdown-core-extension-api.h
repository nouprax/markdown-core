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
 *   for opening and closing nodes, then atomically consume a
 *   delimiter marker with markdown_core_inline_parser_consume_delimiter.
 *   This is the technique that would be used if emphasis
 *   inlines were implemented as an extension.
 *
 * When an extension has consumed delimiter markers,
 * the descriptor's
 * 'insert_inline_from_delim' function
 * will get called in a later phase,
 * when the inline parser has matched opener and closer delimiters
 * created by the extension together.
 *
 * The callback receives an immutable match snapshot and may only update the
 * AST. Delimiter topology and range retirement remain exclusively owned by
 * the engine.
 *
 * Reducers are transactional at the AST boundary: every operation that can
 * fail, including allocation and semantic validation, must finish before the
 * first AST mutation. A reducer that has mutated the AST must return
 * MARKDOWN_CORE_DELIMITER_OK. The engine can always restore delimiter
 * topology, but deliberately does not clone or roll back the AST.
 *
 * Finally, the extension should return NULL if its scan didn't
 * match its syntax rules.
 *
 */
typedef struct subject markdown_core_inline_parser;

/*
 * Source trigger bytes, delimiter rule identity, and extension ownership are
 * deliberately separate namespaces. A rule is immutable descriptor data;
 * the parser binds it to a dense parser-local lane when the extension is
 * attached.
 */
typedef enum {
    MARKDOWN_CORE_DELIMITER_PAIR_NEAREST = 0,
    MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK = 1,
} markdown_core_delimiter_pairing;

typedef enum {
    /* Consume the complete matched range, including both endpoints. */
    MARKDOWN_CORE_DELIMITER_REDUCE_RANGE = 0,
    /* Consume only the matched endpoints, preserving interior delimiters. */
    MARKDOWN_CORE_DELIMITER_REDUCE_ENDPOINTS = 1,
    /* Consume one or two marker bytes and keep nonempty endpoint runs live. */
    MARKDOWN_CORE_DELIMITER_REDUCE_RUN = 2,
} markdown_core_delimiter_reduction;

typedef enum {
    MARKDOWN_CORE_DELIMITER_OK = 0,
    MARKDOWN_CORE_DELIMITER_OOM = 1,
    MARKDOWN_CORE_DELIMITER_INVALID = 2,
} markdown_core_delimiter_result;

/*
 * A shared-close probe is a pure lexical query over immutable source bytes.
 * Zero means the rule cannot close at `offset`; a positive result is the
 * exact byte length core must consume for the closing marker.
 */
typedef markdown_core_bufsize (*markdown_core_delimiter_close_probe_func)(
    uint16_t kind,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset
);

typedef struct {
    markdown_core_delimiter_pairing pairing;
    markdown_core_delimiter_reduction reduction;
    /* Must be nonzero exactly when close_probe is non-NULL. */
    unsigned char close_trigger;
    markdown_core_delimiter_close_probe_func close_probe;
} markdown_core_delimiter_rule;

typedef struct {
    uint16_t kind;
    markdown_core_node *opener_node;
    markdown_core_node *closer_node;
    markdown_core_bufsize opener_start;
    markdown_core_bufsize opener_end;
    markdown_core_bufsize closer_start;
    markdown_core_bufsize closer_end;
    markdown_core_bufsize opener_length;
    markdown_core_bufsize closer_length;
    markdown_core_bufsize opener_remaining;
    markdown_core_bufsize closer_remaining;
    markdown_core_bufsize use_length;
    /* The endpoints' concrete capture handles (opaque, 0 when the engine
     * runs uncaptured). A RANGE reducer that actually consumed its
     * endpoints passes the match back through
     * markdown_core_inline_parser_concrete_use_endpoints; RUN and
     * ENDPOINTS reductions are recorded by the engine itself. */
    uint32_t opener_concrete;
    uint32_t closer_concrete;
} markdown_core_delimiter_match;

/** Records that a RANGE reduce consumed both endpoint delimiter runs as
 * markup. Call exactly on the reducer's success path — a reduce that
 * returns OK without changing the tree must not call it, which is why the
 * engine cannot infer this for RANGE shapes. */
void markdown_core_inline_parser_concrete_use_endpoints(
    markdown_core_inline_parser *parser,
    const markdown_core_delimiter_match *match
);

/** Records that a reduce re-interpreted [start, end) of the unit's content
 * as raw source — a formula or cross-reference body whose parsed interior
 * nodes it discarded. Every concrete record lying wholly inside the span
 * is retracted, because the spelling it claimed to consume is preserved
 * verbatim again. */
void markdown_core_inline_parser_concrete_reinterpret(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize start,
    markdown_core_bufsize end
);

/** Records [start, end) of the unit's content as markup spelling an
 * extension consumed outside the delimiter engine — an inline directive's
 * `:name` or its nameside `{attrs}` — fully consumed under `kind` (a
 * markdown_core_inline_concrete_kind). Call exactly on the consume's
 * success path, after the markdown_core_inline_parser_consume_source that
 * advanced past the bytes; a lost record joins the parse's sticky
 * failure, so the parse is discarded rather than published thinner. The
 * spelling's trigger byte must already be a seam barrier (an extension's
 * unconditional special char is one), which is what keeps the inert
 * prefix of an incremental seam fast-forward recordless. */
void markdown_core_inline_parser_concrete_capture_spelling(
    markdown_core_inline_parser *parser,
    uint8_t kind,
    markdown_core_bufsize start,
    markdown_core_bufsize end
);

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

typedef markdown_core_delimiter_result (*markdown_core_inline_from_delim_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
);

/**
 * Materializes an extension-owned inline node after delimiter reduction has
 * discarded every opaque node hidden by an outer match. The callback must be
 * idempotent and may only update `node`'s payload. Return nonzero on success
 * (including when no work is required) and zero on allocation failure.
 */
typedef int (*markdown_core_materialize_inline_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *node
);

/** Should return 'true' if 'input' can be contained in 'container',
 *  'false' otherwise.
 */
/* Whether an open block continues on this line, with three outcomes:
 *
 *   > 0  it continues;
 *   = 0  it does not match — its prefix is absent. A paragraph inside it may
 *        still continue lazily, the way one continues out of a block quote;
 *   < 0  it ends here, having consumed its own terminator. The engine
 *        finalizes it at once and stops processing the line, which is what a
 *        closing fence means and what keeps the next line from continuing
 *        content the terminator already closed.
 *
 * An extension that only ever continues or fails to match returns 0 and 1 and
 * needs to know nothing about the third. */
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

/**
 * Builds a detached shell for an extension-owned inline owner whose complete
 * child list can be reparsed when a reference-definition answer changes.
 * Before refinement the shell owns a copy of the owner's raw inline source;
 * after refinement its complete child list is the replacement domain.
 *
 * The returned shell must have the same semantic node type, extension
 * descriptor, and allocator as `committed_owner`; refinement must not replace
 * it. NULL reports allocation failure. An ownership domain is always the
 * complete child list of one real semantic owner, never an optimization
 * subrange or a prefix selected by syntax-specific policy.
 */
typedef markdown_core_node *(*markdown_core_prepare_inline_domain_func)(
    markdown_core_extension *extension,
    const markdown_core_node *committed_owner
);

/** Block-local postprocess hook. After inline parsing, footnote processing,
 * and per-block text consolidation, the parser calls this once for every
 * block (and every inline-owning node, such as a table cell or directive
 * label) in document order. All effects must stay inside that node's subtree
 * so the pipeline can later rerun for single blocks.
 *
 * Returns the node now occupying the block's position in the tree: the
 * block itself, or its replacement when the extension replaced or retyped
 * the block. Must not return NULL. */
typedef markdown_core_node *(*markdown_core_postprocess_block_func)(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *block
);

/* Mixes this node's extension-owned VALUE-bearing scalars into `h` and
 * returns the result. An extension that registers a node type whose content
 * lives outside the core union MUST implement this: the diff pairs nodes on
 * `subtree_hash`, so two nodes the hash cannot tell apart are paired, and the
 * newcomer is handed the survivor's identity. Mix whatever
 * markdown_core_ast_parts_changed compares for the type — a directive's name
 * and attributes, a table's alignments, a row's header bit, a formula's mode.
 * Use markdown_core_hash_mix and markdown_core_hash_bytes so every type is
 * sampled the same way. */
typedef uint64_t (*markdown_core_hash_value_func)(
    markdown_core_extension *extension,
    const markdown_core_node *node,
    uint64_t h
);

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

/** Set the syntax extension responsible for creating 'node'.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_extension(markdown_core_node *node, markdown_core_extension *extension);

/**
 * ## Inline syntax extension helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

/** Get the current inline parsing offset */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_get_offset(markdown_core_inline_parser *parser);

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

typedef struct {
    int start_line;
    int start_column;
    int end_line;
    int end_column;
} markdown_core_inline_source_span;

/**
 * Atomically consumes `[current_offset, end_offset)` and returns its precise
 * source scope. Invalid or non-forward ranges poison the parse as an internal
 * error and leave the cursor unchanged.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_inline_parser_consume_source(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize end_offset,
    markdown_core_inline_source_span *span
);

/**
 * Creates a borrowed Text node for `[current_offset, end_offset)` and commits
 * the cursor only after node allocation succeeds.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_inline_parser_consume_text(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize end_offset
);

/**
 * Creates and publishes one delimiter marker as a single transaction. Rule
 * identity comes from the currently executing extension attachment; source
 * bounds, run length, scope, and ownership are derived by core.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_inline_parser_consume_delimiter(
    markdown_core_inline_parser *parser,
    uint16_t kind,
    int can_open,
    int can_close,
    markdown_core_bufsize end_offset
);

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
 * Returns the number of delimiters encountered, in the limit of
 * 'max_delims'. This is a pure lookahead and does not advance the parser.
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

/**
 * Compatibility no-op. Attached inline grammar is now compiled into an
 * always-active parser-local plan and no longer requires temporary bitmap
 * mutation.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_manage_extensions_special_characters(markdown_core_parser *parser, int add);

#ifdef __cplusplus
}
#endif

#endif
