#ifndef MARKDOWN_CORE_ELEMENT_API_H
#define MARKDOWN_CORE_ELEMENT_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include "node_type.h"
#include "buffer.h"
#include <stdbool.h>

typedef struct markdown_core_parser markdown_core_parser;
typedef struct markdown_core_element markdown_core_element;
/* A node being built, and its place among the nodes being built (node.h). */
typedef struct markdown_core_member markdown_core_member;

/* Where a depth-first walk stands on a node: every node yields one ENTER and
 * one EXIT, and DONE follows the root's EXIT (iterator.h). Element completion
 * steps receive the event they are called at. */
typedef enum {
    MARKDOWN_CORE_EVENT_NONE,
    MARKDOWN_CORE_EVENT_DONE,
    MARKDOWN_CORE_EVENT_ENTER,
    MARKDOWN_CORE_EVENT_EXIT
} markdown_core_event_type;

struct markdown_core_chunk;

/* A borrowed slice of immutable parser-owned source runs. It owns no AST node
 * or allocation and may be copied while a node is split or consolidated. */
typedef struct {
    int first, count, offset;
} markdown_core_content_map;

/** Internal element parsing API.
 *
 * Every parse attaches the complete immutable element table. Elements own
 * lexical recognition, node construction and element-specific state; shared
 * block and inline engines own traversal, dispatch, source mapping and
 * delimiter reduction. This is private implementation machinery, not a public
 * mechanism for selecting or extending the dialect.
 *
 * Block callbacks continue existing containers and recognize new blocks in
 * descriptor order. A declined opener returns NULL without consuming input.
 *
 * Inline callbacks receive markdown_core_inline_state at the current source
 * offset. They may construct a token directly or push delimiters for the
 * shared engine to reduce. Parsed containers use the shared rule constructor;
 * opaque-body elements provide only their construction and decoding hook.
 * A callback may consume an internal token without returning a node. A decline
 * leaves both the cursor and the tree unchanged.
 * A returned token is exclusively owned and detached. Its constructor must
 * guarantee the fixed grammar's built-in containment for the receiving
 * parent; the dispatcher commits that proof and checks it in Debug/ASan.
 * An optional dynamic parent policy is a separate decision made by the
 * dispatcher for each returned token, with refusal releasing the token.
 * Private test descriptors have the same constructor obligations. Registering
 * a descriptor does not turn this internal API into a supported extension API.
 *
 * Element state follows the descriptor's parser, inline and node lifecycle
 * callbacks. Owned subtree roots participate in the shared iterative walks.
 */
typedef struct markdown_core_inline_state markdown_core_inline_state;

/** One element as one parse holds it (see "AN ELEMENT AS ONE PARSE HOLDS IT"
 * below). Every parse-time hook is handed its own as `self`. */
typedef struct markdown_core_element_instance markdown_core_element_instance;

/** The dialect of one parser instance while its setup extends it
 * (dialect.h). Setup is the only code that holds one. */
typedef struct markdown_core_dialect_builder markdown_core_dialect_builder;

/** A delimiter names its RULE, not a byte.
 *
 * It used to carry an `unsigned char delim_char`, and three separate things
 * were derived from that byte:
 *
 *   WHO OWNS IT -- `get_element_for_special_char` walked the attached
 *   elements and returned the first whose dispatch set contained the byte.
 *   Two elements may claim one byte (`autolink` and `directive` both claim
 *   `:`), so the answer was attach order; and if no element claimed it the
 *   answer was NULL, which `process_emphasis`'s `else if` chain then fell
 *   straight through -- **without advancing the cursor** -- freeing the
 *   delimiter and reading it again on the next turn. Measured: ASan
 *   `heap-use-after-free`, READ of size 1 in `process_emphasis`, from an
 *   element that pushes a byte it does not itself dispatch; with
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
    MARKDOWN_CORE_DELIM_RULE_EMPHASIS,    /* `*` */
    MARKDOWN_CORE_DELIM_RULE_UNDERSCORE,  /* `_` */
    MARKDOWN_CORE_DELIM_RULE_MARK,        /* `==`, pairwise, no rule of three */
    MARKDOWN_CORE_DELIM_RULE_INSERTION,   /* `++`, pairwise, no rule of three */
    MARKDOWN_CORE_DELIM_RULE_SUPERSCRIPT, /* `^`, empty allowed, no raw whitespace */
    MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT,   /* `~`, single-run units, no raw whitespace */
    /* Elements. One entry per rule, not per element and not per byte. */
    MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_INLINE,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_DISPLAY,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_INLINE,
    MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_DISPLAY,
    MARKDOWN_CORE_DELIM_RULE_DIRECTIVE_LABEL,
    /* A rule id also names an opaque-body search (see
     * markdown_core_inline_state_find_opaque_close): the `%%` comment pushes
     * no delimiter, but its closer search caches its failures under this id. */
    MARKDOWN_CORE_DELIM_RULE_COMMENT,
    MARKDOWN_CORE_DELIM_RULE_COUNT
} markdown_core_delimiter_rule;

/** The delimiter stack's element, OPAQUE.
 *
 * Elements only receive marker entries through the construction hook.
 * Source boundaries and token-completion events are private to the engine;
 * elements cannot traverse or mutate the stack.
 */
typedef struct delimiter delimiter;

/** The member of the literal text node the delimiter was pushed for. */
markdown_core_member *markdown_core_delimiter_member(const delimiter *delim);

markdown_core_delimiter_rule markdown_core_delimiter_rule_of(const delimiter *delim);

/** The inline state offset just past the delimiter's last byte. */
bufsize_t markdown_core_delimiter_position(const delimiter *delim);

/** How many bytes the delimiter run owns. */
bufsize_t markdown_core_delimiter_length(const delimiter *delim);

int markdown_core_delimiter_can_open(const delimiter *delim);

int markdown_core_delimiter_can_close(const delimiter *delim);

/** Should create and add a new open block to 'parent_container' if
 * 'input' matches a syntax rule for that block type. It is allowed
 * to modify the type of 'parent_container'.
 *
 * Should return the newly created block's member if there is one, or
 * 'parent_container' if its type was modified, or NULL.
 */
typedef markdown_core_member *(*markdown_core_open_block_func)(const markdown_core_element_instance *self, int indented,
                                                               markdown_core_parser *parser,
                                                               markdown_core_member *parent_container,
                                                               unsigned char *input, int len);

/** Returns the member of the token it appended to 'parent' with
 * markdown_core_inline_state_append, or NULL. */
typedef markdown_core_member *(*markdown_core_match_inline_func)(const markdown_core_element_instance *self,
                                                                 markdown_core_parser *parser,
                                                                 markdown_core_member *parent, unsigned char character,
                                                                 markdown_core_inline_state *inline_state);

/* Builds the opaque AST value only. The matcher owns all delimiter removal,
 * including the matched endpoints, on success and failure alike. */
typedef void (*markdown_core_inline_from_delim_func)(const markdown_core_element_instance *self,
                                                     markdown_core_parser *parser,
                                                     markdown_core_inline_state *inline_state, delimiter *opener,
                                                     delimiter *closer);

/** Returned by a 'markdown_core_match_block_func' when 'input' is the
 *  container's own closing line.
 *
 *  The parser closes the container and every block still open inside it, ends
 *  the container at THIS line, and stops processing the line. Returning 1 and
 *  consuming the fence is not enough: the container stays open, and the next
 *  non-blank line is taken as a lazy paragraph continuation and pulled inside
 *  it, on the wrong line.
 *
 *  0 and 1 keep their meanings, so an element that never returns this is
 *  unaffected.
 */
#define MARKDOWN_CORE_BLOCK_CLOSED 2
/* A DirectiveBlock fence competes with deeper containers and opaque blocks. Leave
 * the cursor unchanged: the shared spine walk commits the deepest carried
 * container's closer only after descendant ownership is known. Both matching
 * and non-consuming continuation hooks may return this result. */
#define MARKDOWN_CORE_BLOCK_PENDING_CLOSE 3

/** Should return 'true' if 'input' can be contained in 'container',
 *  'false' otherwise, or MARKDOWN_CORE_BLOCK_CLOSED if 'input' is the
 *  container's own closing line. A DirectiveBlock returns
 *  MARKDOWN_CORE_BLOCK_PENDING_CLOSE until descendant ownership is known.
 */
typedef int (*markdown_core_match_block_func)(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                              unsigned char *input, int len, markdown_core_member *container);

/** Whether 'input' would continue 'container', asked AHEAD OF TIME.
 *
 *  A block start may look at the lines after its own before it opens (see
 *  markdown_core_parser_lookahead_begin in the core), and it then asks every
 *  open container whether each later line carries its prefix. That question
 *  must leave no trace: 'last_block_matches' consumes the line, closes the
 *  container on its fence and records state on the node, so it cannot be
 *  asked speculatively. An element that opens block CONTAINERS provides this
 *  hook as the same test with none of those effects: 1 when 'input' continues
 *  'container', 0 when it does not or is the container's own closing line. It
 *  must not change the parser beyond the cursor fields the core resets, the
 *  container, or any node. A container whose element provides no hook ends
 *  every lookahead at its next line.
 */
typedef int (*markdown_core_continues_block_func)(const markdown_core_element_instance *self,
                                                  markdown_core_parser *parser, const unsigned char *input, int len,
                                                  markdown_core_member *container);

typedef int (*markdown_core_can_contain_func)(const markdown_core_element *element, markdown_core_node *node,
                                              markdown_core_node_type child);

typedef int (*markdown_core_contains_inlines_func)(const markdown_core_element *element, markdown_core_node *node);

typedef int (*markdown_core_accepts_lines_func)(const markdown_core_element *element, markdown_core_node *node);

/** COMPLETION STEPS: an inline root completes its own tree when its parse
 * ends (docs/plans/2026-09-29-incremental-parsing.md, 5.8), in one pass over
 * that tree -- the content of a paragraph, a heading, a cell, or of a field
 * such as a definition term, a callout title, a table caption or a directive
 * label, with the fields of the inline nodes in it. The pass consolidates each
 * run of Text, completes each node, numbers what each node holds when the node
 * is left, and asks an element's completion step at the events of the kinds it
 * declared (`complete_exit_kinds`, `complete_scope_kinds`).
 *
 * A step is LOCAL to the node it is handed. At EXIT it may read and rewrite
 * the current node and its complete subtree; at ENTER the subtree is untouched
 * and about to be passed. It may READ the siblings that FOLLOW the current
 * node, but never unlink, move or free one of them: the pass's lookahead
 * already names the node after the current one. It may free only the node
 * whose EXIT is current, and only when that node owns no field roots; it may
 * insert only BEFORE the current node, which the pass has left and never
 * visits again, and what it inserts it completes itself
 * (`markdown_core_parser_complete_node`). A step never walks anything itself.
 */

/** What a completion step did to the current node. */
typedef enum {
    /** The node is still in the tree; the steps after this one run. */
    MARKDOWN_CORE_COMPLETE_CONTINUE,
    /** The node was freed or replaced. No later step sees this event: the
     *  node it names is gone. Legal only at EXIT. */
    MARKDOWN_CORE_COMPLETE_CONSUMED,
    /** An allocation failed and 'parser->error' is set. The pass stops. */
    MARKDOWN_CORE_COMPLETE_FAILED
} markdown_core_complete_result;

/** Observe one event of an inline root's completion at 'node'.
 *
 * 'event' is `MARKDOWN_CORE_EVENT_EXIT` for a node of a kind the step declared
 * in `complete_exit_kinds` (asked once the node's subtree is complete), and
 * `MARKDOWN_CORE_EVENT_ENTER` or `MARKDOWN_CORE_EVENT_EXIT` for a node of a
 * kind it declared in `complete_scope_kinds` (the kinds whose extent it
 * tracks). It is asked at no other event, and at the EXIT of a kind it is
 * asked at only once the parse has produced a kind of those it declared in
 * `complete_acts_on_kinds`, the kinds it acts on. The ENTER and EXIT of a
 * scope kind are delivered whenever the extent is passed, so the state a
 * step keeps for an extent is always in step with the tree.
 * 'is_root' is 1 when 'node' is the root of a field -- a definition's term,
 * a callout's title -- which belongs to its owner and may be rewritten in
 * place but never replaced or freed. '*state' is one word the pass keeps for
 * this element per root, zero when the root's pass starts, so a step can
 * carry a fact such as "inside a Link" across the events of one root and
 * never across roots.
 *
 * The step obeys the LOCAL contract above. It returns CONSUMED when it freed
 * or replaced 'node' (legal only at EXIT), FAILED with 'parser->error' set when
 * an allocation failed, and CONTINUE otherwise.
 */
typedef markdown_core_complete_result (*markdown_core_complete_step_func)(const markdown_core_element_instance *self,
                                                                          markdown_core_parser *parser,
                                                                          markdown_core_member *node,
                                                                          markdown_core_event_type event, int is_root,
                                                                          void **state);

typedef void (*markdown_core_opaque_alloc_func)(const markdown_core_element *element, markdown_core_node *node);

typedef void (*markdown_core_opaque_free_func)(const markdown_core_element *element, markdown_core_node *node);

/** A parser element is a `static const` descriptor in a fixed compile-time
 * table (`elements/core-elements.c`), not an object built at run time.
 *
 * The inherited API used to expose a constructor, sixteen setters, a `free`,
 * and a `priv`/`free_function` pair no element in this repository ever used.
 * Between them they made the descriptor mutable, heap-allocated from a hidden
 * process-global allocator, and reachable only through a process-global
 * registry keyed by name. All of it is gone: every hook takes a `const`
 * descriptor, so "carries no mutable state" is a fact the compiler checks
 * rather than a convention.
 */

/** AN ELEMENT AS ONE PARSE HOLDS IT.
 *
 * A descriptor is immutable, so what an element learns during a parse lives
 * in records the engine gives it. The descriptor declares their sizes:
 *
 *   `state_size`     -- one record per parse transaction, zeroed before the
 *                       document lifecycle begins and released when the
 *                       transaction ends. The element's `dispose_parser`
 *                       releases whatever the record owns.
 *   `run_state_size` -- one record per inline-content run, zeroed before the
 *                       run's `init_inline` hooks and released after its
 *                       `dispose_inline` hooks, which release whatever the
 *                       record owns. A run started without a parser (a
 *                       reference definition's cursor) has none.
 *
 * Sealing a dialect makes one INSTANCE of each element it holds: the
 * descriptor, its parse record and where its run record lies in a run. The
 * engine hands every parse-time hook its own instance as `self`, so an
 * element reaches its own state directly: `self->state`, and
 * `markdown_core_run_state` for the run's. Hooks that act on nodes outside a
 * parse (`can_contain_func` and the rest) take the descriptor alone.
 *
 * An element whose code reads ANOTHER element's state declares that element
 * in `peers`. Sealing resolves each declared peer once, in declaration order,
 * into `self->peers`: the peer's instance, or NULL when the dialect does not
 * hold it. The declaration is where one element depending on another is
 * written down, and no parse-time code looks an element up by name. The
 * engine never reads a record; it knows sizes and lifetimes only, so an
 * element that needs state declares a size rather than adding a field to a
 * core struct.
 */
struct markdown_core_element_instance {
    const markdown_core_element *element;
    /* The parse record, or NULL when the element declares none. */
    void *state;
    /* Where the element's record lies in a run's block of records. */
    size_t run_offset;
    /* The instances of the descriptor's `peers`, in its order. */
    const markdown_core_element_instance *const *peers;
};

/** The instance of `element` in the parse `parser` runs, or NULL when its
 *  dialect does not hold `element`. A dialect holds an element once:
 *  registration refuses a descriptor already attached. For code outside the
 *  dialect -- an embedder or a test inspecting a parse; an element reaches
 *  another through its declared `peers`. */
const markdown_core_element_instance *markdown_core_parser_instance(const markdown_core_parser *parser,
                                                                    const markdown_core_element *element);

/** Return the index of the line currently being parsed, starting with 1.
 */
int markdown_core_parser_get_line_number(markdown_core_parser *parser);

/** Return the offset in bytes in the line being processed.
 *
 * Example:
 *
 * ### foo
 *
 * Here, offset will first be 0, then 5 (the index of the 'f' character).
 */
int markdown_core_parser_get_offset(markdown_core_parser *parser);

/**
 * Return the offset in 'columns' in the line being processed.
 *
 * This value may differ from the value returned by
 * markdown_core_parser_get_offset() in that each complete Unicode scalar
 * counts as one column and tabs expand to the next multiple of four. This
 * value should not be used as an index in the current line's
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
int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser);

/** Declare that 'node''s content -- which the caller SET rather than the parser
 * parsing it -- begins at (line, column) in the source, and runs on from there
 * without a break. Returns 1, or 0 if it could not be recorded, in which case
 * the parse is marked lost.
 *
 * A block whose content the parser copied in line by line gets this from
 * `add_line`. A block whose content an element handed it -- a table cell cut
 * out of a row, a directive's label -- has none, and every position inside it
 * then falls back to arithmetic on the block's own start column, which is right
 * only while the content is one line beginning where the block does. One mark
 * is the whole answer for content that is one line long, which is what all of
 * those are.
 */
/** Share the immutable marks covering [from, from + length) of 'owner''s
 * content with 'node', with its content origin at 'from'. Returns 1, or 0
 * when there is nothing to map. This does not allocate.
 *
 * For content that is a SLICE of another block's content and more than one line
 * long -- the paragraph a table was split out of -- where one mark would put
 * every line of it on the first line's row.
 */
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, const markdown_core_content_map *owner,
                                             markdown_core_content_map *map, bufsize_t from, bufsize_t length);

int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line,
                                      bufsize_t source);

/** Name the source line, counted from 1, and the source byte offset of the
 * byte at 'content_offset' in 'node''s content buffer, and return 1. Returns 0,
 * leaving both outputs untouched, for a node that never took a line.
 *
 * A block's content is the concatenation of the line slices the parser copied
 * into it with the container prefix stripped, so an offset in it is NOT a
 * column: `"> foo\nbar"` strips two bytes from the first line and none from
 * the second, and the two lines of one paragraph's content then start at
 * different distances from their line starts. This is the only thing that knows which.
 *
 * The map is live for as long as the parse is: an element may ask while the
 * block is open, and the inline phase may ask after every block has closed.
 * the parse transaction releases it with the rest of the parse state.
 */
int markdown_core_parser_content_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                       bufsize_t content_offset, int *line, bufsize_t *source);

/** Append a source run for content already assembled by a producer. Runs
 * must be contiguous in the parser vector and have increasing content offsets.
 * source_width is the authored width represented by each logical byte, and
 * source_step is the source-byte stride. Allocation failure marks the parse lost. */
int markdown_core_parser_append_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                             int line, bufsize_t source, int source_width, int source_step);
/** Append the source runs covering a literal slice to a growing result map.
 * Each source run is copied once; producers use adopt_content_marks for a
 * read-only slice that needs no allocation. */
int markdown_core_parser_append_content_marks(markdown_core_parser *parser, const markdown_core_content_map *owner,
                                              markdown_core_content_map *map, bufsize_t from, bufsize_t length,
                                              bufsize_t offset);
/** Project a logical inline range, including its Text literal mapping. */
void markdown_core_inline_state_place(markdown_core_inline_state *inline_state, markdown_core_node *node, int from,
                                      int to);
/** The exclusive source end of the authored bytes represented by a content
 * byte. Uses the same run lookup as content_place, which returns its start. */
int markdown_core_parser_content_end_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                           bufsize_t offset, int *line, bufsize_t *end);

/** Return the absolute index of the first nonspace column coming after 'offset'
 * in the line currently being processed, counting tabs as multiple
 * columns as appropriate.
 *
 * See the documentation for markdown_core_parser_get_first_nonspace() and
 * markdown_core_parser_get_column() for more information.
 */
int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser);

/** Return the difference between the values returned by
 * markdown_core_parser_get_first_nonspace_column() and
 * markdown_core_parser_get_column().
 *
 * This is not a byte offset, as it can count one tab as multiple
 * characters.
 */
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
int markdown_core_parser_is_blank(markdown_core_parser *parser);

/** Return 'true' if the value returned by markdown_core_parser_get_offset()
 * is 'inside' an expanded tab.
 *
 * See the documentation for markdown_core_parser_get_column() for more
 * information.
 */
int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser);

/** Return the source offset just after the previously processed line's last
 * byte, excluding its line ending: the line's start for an empty line. In a
 * table cell's content, which the parser reads as an input of its own, it is
 * the end of the cell line's last byte in the source line.
 */
bufsize_t markdown_core_parser_get_last_line_end(markdown_core_parser *parser);

/** Add a child to 'parent' during the parsing process, and return its
 * member.
 *
 * If 'parent' isn't the kind of node that can accept this child,
 * this function will back up till it hits a node that can, closing
 * blocks as appropriate.
 */
markdown_core_member *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_member *parent,
                                                     markdown_core_node_type block_type, int start_column);

/** A member for the detached `node`, holding its reference, linked under
 * `owner` before `before` or last; NULL, with the parse failed and the node
 * released, when it could not be allocated. The caller has proved
 * containment. */
markdown_core_member *markdown_core_parser_attach(markdown_core_parser *parser, markdown_core_member *owner,
                                                  markdown_core_node *node, markdown_core_member *before);

/** A member for the field root `node`, which `owner`'s node holds, linked as
 * the last field root `owner` builds; NULL, with the parse failed, when it
 * could not be allocated. */
markdown_core_member *markdown_core_parser_attach_field(markdown_core_parser *parser, markdown_core_member *owner,
                                                        markdown_core_node *node);

/** The node that holds `member`'s node: its owner's, or, for the builder of
 * the inline root being completed, the node that holds its holder. NULL for
 * a root. */
markdown_core_node *markdown_core_parser_owner(const markdown_core_parser *parser, const markdown_core_member *member);

/** Detaches `member` from its owner and siblings when it has them, and
 * releases it, its subtree and the references they hold into the parse's
 * pool. */
void markdown_core_parser_release_member(markdown_core_parser *parser, markdown_core_member *member);

/** Complete 'member''s node (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.8): its children, each complete, become its stem, and it numbers each
 * node it holds that is not numbered yet, measured from where it starts. The
 * engine completes every block as it closes and every node of an inline
 * root's content as the root's completion leaves it; an element completes
 * what it makes outside both, such as the nodes a completion step inserts.
 */
void markdown_core_parser_complete_node(markdown_core_parser *parser, markdown_core_member *member);

/** Advance the 'offset' of the parser in the current line.
 *
 * See the documentation of markdown_core_parser_get_offset() and
 * markdown_core_parser_get_column() for more information.
 */
void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns);

/** Register 'element' after every element 'builder' already holds.
 *  See the documentation for markdown_core_element for more information.
 *
 *  Returns 'true' if the 'element' was registered, 'false' otherwise: on
 *  allocation failure, for a descriptor the registration rule refuses -- one
 *  where a completion step is asked without a step, or a step asked at no
 *  kind, or one kind as both an exit and a scope kind, or only part of the
 *  document lifecycle, or a flanking-transparent byte outside ASCII -- and once the dialect holds
 *  255 elements (the block-start projection lists a family's owners by byte),
 *  with the builder left as it was.
 */
int markdown_core_dialect_builder_attach(markdown_core_dialect_builder *builder, const markdown_core_element *element);

/** The elements 'builder' holds, in registration order; their number in
 *  '*count'. The view is valid until the next registration. */
const markdown_core_element *const *markdown_core_dialect_builder_elements(const markdown_core_dialect_builder *builder,
                                                                           size_t *count);

/** A parse's dialect extension: it receives the builder, already holding the
 *  parse's base elements, before any source is read. Returning false aborts
 *  the parse. */
typedef bool (*markdown_core_parser_setup_func)(markdown_core_dialect_builder *builder, void *context);

typedef enum {
    MARKDOWN_CORE_NODE_SET_KIND_OK,
    MARKDOWN_CORE_NODE_SET_KIND_REJECTED,
    MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED,
} markdown_core_node_set_kind_result;

/** Change 'node', held by 'owner', to the internal kind encoded by 'kind'.
 *
 * Return OK on success, REJECTED when the owner's containment disallows it,
 * or ALLOCATION_FAILED when replacement node data cannot be allocated.
 * Either failure preserves the original kind, data, and tree links.
 *
 * A change releases values owned by the old kind and installs the new kind's
 * defaults. Node identity and element-owned opaque data are preserved.
 * A record that fits the node's existing cell needs no allocation.
 * Setting the current kind succeeds without allocating or changing its data.
 */
markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node *owner,
                                                               markdown_core_node_type kind);

/** Return the string content for all types of 'node'.
 *  The pointer stays valid as long as 'node' isn't freed.
 */
const char *markdown_core_node_get_string_content(markdown_core_node *node);

/** Set the string 'content' for all types of 'node'.
 *  Copies 'content'.
 */
int markdown_core_node_set_string_content(markdown_core_node *node, const char *content);

/** Set the parser element responsible for creating 'node'.
 */
int markdown_core_node_set_element(markdown_core_node *node, const markdown_core_element *element);

/**
 * ## Inline parser element helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

/** Should return 'true' if the predicate matches 'c', 'false' otherwise
 */
typedef int (*markdown_core_inline_predicate)(int c);

/** Advance the current inline parsing offset */
void markdown_core_inline_state_advance_offset(markdown_core_inline_state *inline_state);

/** Get the current inline parsing offset */
int markdown_core_inline_state_get_offset(markdown_core_inline_state *inline_state);

/** Set the offset in bytes in the chunk being processed by the given inline state.
 */
void markdown_core_inline_state_set_offset(markdown_core_inline_state *inline_state, int offset);

/** Gets the markdown_core_chunk being operated on by the given inline state.
 * Use markdown_core_inline_state_get_offset to get our current position in the chunk.
 */
struct markdown_core_chunk *markdown_core_inline_state_get_chunk(markdown_core_inline_state *inline_state);

/** Remove the last n characters from the last children of the given member,
 * while they are MARKDOWN_CORE_NODE_TEXT.
 */
void markdown_core_node_unput(markdown_core_parser *parser, markdown_core_member *member, int n);

/** Get the character located at the current inline parsing offset
 */
unsigned char markdown_core_inline_state_peek_char(markdown_core_inline_state *inline_state);

/** Get the character located 'pos' bytes in the current line.
 */
unsigned char markdown_core_inline_state_peek_at(markdown_core_inline_state *inline_state, int pos);

/** Whether the inline state has reached the end of the current line
 */
int markdown_core_inline_state_is_eof(markdown_core_inline_state *inline_state);

/** Get the characters located after the current inline parsing offset
 * while 'pred' matches. Free after usage.
 */
char *markdown_core_inline_state_take_while(markdown_core_inline_state *inline_state,
                                            markdown_core_inline_predicate pred);

/* A delimiter scanner describes one lexical unit: its width and whether it
 * closes this rule. The shared cursor caches failed suffix searches per rule,
 * then treats a recognized body as raw text until the closing delimiter. The
 * owning element still constructs its node through the delimiter stack. */
typedef int (*markdown_core_opaque_delimiter_scanner)(const unsigned char *data, int length, int offset,
                                                      markdown_core_delimiter_rule rule, bool *closes);
void markdown_core_inline_state_set_opaque_body_end(markdown_core_inline_state *inline_state, int end);
int markdown_core_inline_state_find_opaque_close(markdown_core_inline_state *inline_state,
                                                 markdown_core_delimiter_rule rule, int from,
                                                 markdown_core_opaque_delimiter_scanner scan);

/** Push a delimiter on the delimiter stack.
 * See <<http://spec.commonmark.org/0.24/#phase-2-inline-structure> for
 * more information on the parameters
 */
void markdown_core_inline_state_push_delimiter(markdown_core_inline_state *inline_state,
                                               const markdown_core_element_instance *owner,
                                               markdown_core_delimiter_rule rule, int can_open, int can_close,
                                               markdown_core_member *inl_text);

/** Whether the delimiters of `rule` on the stack that can open outnumber
 * those that can close. The counts are kept at every push and removal, so the
 * answer costs the same however deep the stack is. For a rule whose closers
 * are pushed only when this answers yes, that is exactly whether an opener is
 * still unmatched: an element whose closers may not stand alone -- a
 * formula's `\\)` is CommonMark's escaped backslash and a parenthesis unless
 * something opened it -- asks here before pushing one, so a closer that would
 * never pair stays with the base language. A rule whose openers may not nest
 * asks the same question before pushing an opener, and its delimiters then
 * alternate on the stack, so every closer pairs with the opener directly
 * before it and no pair ever spans another of the rule.
 */
int markdown_core_inline_state_has_unmatched_opener(markdown_core_inline_state *inline_state,
                                                    markdown_core_delimiter_rule rule);

/** Appends the detached `token` to the content the inline state builds and
 * returns its member, which holds the token's reference. Each field root the
 * token holds is built with it, and a field root with content of its own is
 * parsed before the next token is read. NULL, with the token released and
 * the parse failed, when the owner's policy refuses the token or a member
 * could not be allocated. */
markdown_core_member *markdown_core_inline_state_append(markdown_core_inline_state *inline_state,
                                                        markdown_core_node *token);

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
markdown_core_node *markdown_core_inline_state_make_delimiter_text(markdown_core_inline_state *inline_state, int from,
                                                                   int to);

#ifdef __cplusplus
}
#endif

#endif
