#ifndef MARKDOWN_CORE_ELEMENT_API_H
#define MARKDOWN_CORE_ELEMENT_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/markdown_core.h"
#include "node_type.h"
#include "buffer.h"
#include <stdbool.h>
#include "reader.h"

typedef struct markdown_core_parser markdown_core_parser;
typedef struct markdown_core_element markdown_core_element;
typedef struct markdown_core_line_ops markdown_core_line_ops;

/* Where a depth-first walk stands on a node: every node yields one ENTER and
 * one EXIT, with its children's events between them, and DONE follows the
 * root's EXIT. Element finish steps receive the event they are called at. */
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

/* An absolute byte range [start, end) of the document source (node.h). */
#ifndef MARKDOWN_CORE_PLACE_TYPEDEF
#define MARKDOWN_CORE_PLACE_TYPEDEF
typedef struct markdown_core_place {
    uint32_t start, end;
} markdown_core_place;
#endif

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

/** A paired run names its RULE, not a byte (attention.h): a dense id, so
 * an opener pairs with a closer of its own rule, two rules may share a byte
 * (`$` opens `$x$` and `$$x$$`), and the pass's opener floors are sized by
 * construction.
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
    MARKDOWN_CORE_DELIM_RULE_COUNT
} markdown_core_delimiter_rule;

typedef int (*markdown_core_can_contain_func)(const markdown_core_element *element, markdown_core_node *node,
                                              markdown_core_node_type child);

typedef int (*markdown_core_contains_inlines_func)(const markdown_core_element *element, markdown_core_node *node);

typedef int (*markdown_core_accepts_lines_func)(const markdown_core_element *element, markdown_core_node *node);

/** THE FINISH STEP, and the contract that keeps it local.
 *
 * Every block joins the completion queue as it SETTLES -- as the block that
 * holds it takes its next child or closes, when its own children have
 * settled before it -- and completion takes the queue in that order after
 * the block phase, walking each queued block once -- its ENTER, its inline
 * content and its field roots: a definition term, callout title, citation
 * affix, table caption or directive label -- and its EXIT, stepping over the
 * blocks below it, each queued on its own as it settles. An element takes
 * part in that walk through one step, asked at the events of the kinds it
 * declared (`exits`, `scopes`). A queued block is the
 * root of its walk: a step at its EXIT may rewrite it in place -- its kind,
 * its literal, its record -- and read its block structure, which is final,
 * and never frees or replaces it.
 *
 * Inside a root, the step may touch only what the walk guarantees is
 * settled at that moment: the current node and, at EXIT, the current node's
 * complete subtree (at ENTER the subtree is untouched and about to be
 * walked). It may READ the siblings that FOLLOW the current node, but never
 * unlink, move or free one of them: the walk's lookahead already names the
 * node after the current one. It may free only the node whose EXIT is
 * current, and only when that node owns no field roots (the walk pushed those
 * at its ENTER); it may insert only BEFORE the current node, which the walk
 * has passed and never visits again.
 *
 * A step never walks anything itself; the walk it is part of is the one
 * traversal the finish stage makes.
 */

/** What a finish step did to the current node. */
typedef enum {
    /** The node is still in the tree; the steps after this one run. */
    MARKDOWN_CORE_FINISH_CONTINUE,
    /** The node was freed or replaced. No later step sees this event: the
     *  node it names is gone. Legal only at an inline node's EXIT. */
    MARKDOWN_CORE_FINISH_CONSUMED,
    /** An allocation failed and 'parser->error' is set. The walk stops. */
    MARKDOWN_CORE_FINISH_FAILED
} markdown_core_finish_result;

/** Observe one event of the finish stage at 'node'.
 *
 * 'event' is `MARKDOWN_CORE_EVENT_EXIT` for a node of a kind the step declared
 * in `exits` (asked once the node's subtree is complete), and
 * `MARKDOWN_CORE_EVENT_ENTER` or `MARKDOWN_CORE_EVENT_EXIT` for a node of an
 * inline kind it declared in `scopes` (the kinds whose extent it
 * tracks). It is asked at no other event, and at the EXIT of a kind it is
 * asked at only once the parse has produced a kind of those it declared in
 * `acts`, the kinds it acts on: the gate is read at the
 * event, because completion parses each block's inline content as it reaches
 * the block and a kind's first node may be made after completion began. The
 * ENTER and EXIT of a scope kind are delivered whenever the extent is walked,
 * so the state a step keeps for an extent is always in step with the tree.
 * 'parent' is the node that holds 'node': for a queued block, the block it
 * settled into, NULL for the document; NULL for a field root; and inside a
 * root, the inline node or root whose children it is among. A root belongs to
 * whoever holds it and may be rewritten in place but never replaced or
 * freed. '*state' is one word the walk keeps for this element per root, zero
 * when the root's walk starts, so a step can carry a fact such as "inside a
 * Link" across the events of one root and never across roots.
 *
 * The step obeys the contract above. It returns CONSUMED when it freed or
 * replaced 'node' (legal only at EXIT, and never for a root), FAILED with
 * 'parser->error' set when an allocation failed, and CONTINUE otherwise.
 */
typedef markdown_core_finish_result (*markdown_core_finish_step_func)(const markdown_core_element_instance *self,
                                                                      markdown_core_parser *parser,
                                                                      markdown_core_node *node,
                                                                      markdown_core_node *parent,
                                                                      markdown_core_event_type event, void **state);

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
 *                       run's inline `init` hooks and released after its
 *                       `dispose` hooks, which release whatever the
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
 * An element whose code asks ANOTHER element something asks a service
 * (element.h, markdown_core_service): it names the service in `needs`, and
 * the one that answers names it in `provides`. Sealing resolves each need
 * once, in declaration order, into `self->needs`: the instance of the last
 * element registered that provides it and that element's operations, or
 * both NULL when no element of the dialect does. The declaration is where
 * one element depending on another is written down; no element names
 * another, and no parse-time code looks one up by name. The
 * engine never reads a record; it knows sizes and lifetimes only, so an
 * element that needs state declares a size rather than adding a field to a
 * core struct.
 */
/* Facets (element.h, markdown_core_facet) are kept by public kind, below
 * this many. */
#define MARKDOWN_CORE_FACET_ROOM 64

/* A SERVICE AS A PARSE RESOLVED IT: the instance that provides it and its
 * operations, an interface of elements/services.h, or both NULL when the
 * dialect holds no provider. */
typedef struct markdown_core_need {
    const markdown_core_element_instance *instance;
    const void *ops;
} markdown_core_need;

struct markdown_core_element_instance {
    const markdown_core_element *element;
    /* The element's `lines` (element.h), never NULL: an element whose
     * blocks take no line after their first has the empty table, since
     * every open block is asked of every line it sees. */
    const markdown_core_line_ops *lines;
    /* The parse record, or NULL when the element declares none. */
    void *state;
    /* Where the element's record lies in a run's block of records. */
    size_t run_offset;
    /* The services the descriptor `needs`, in its order. */
    const markdown_core_need *needs;
    /* The dialect that holds it, which answers what the dialect's other
     * kinds show (markdown_core_instance_facet). */
    const struct markdown_core_dialect *dialect;
    /* The label space numbers of the descriptor's `spaces`, in its order
     * (element.h, markdown_core_space). */
    const unsigned *spaces;
    /* Where the element's `mine` rules for closing a label lie among the
     * dialect's (dialect.h, `closers`), and how many there are. */
    uint32_t mine_first, mine_count;
    /* The kind of the element's first token kind (element.h,
     * markdown_core_inline_ops `tokens`), and of its first block token kind
     * (markdown_core_open_ops `tokens`). */
    uint16_t token_base, block_base;
};

/** The instance of `element` in the parse `parser` runs, or NULL when its
 *  dialect does not hold `element`. A dialect holds an element once:
 *  registration refuses a descriptor already attached. For code outside the
 *  dialect -- an embedder or a test inspecting a parse; an element reaches
 *  another through the services it `needs`. */
const markdown_core_element_instance *markdown_core_parser_instance(const markdown_core_parser *parser,
                                                                    const markdown_core_element *element);

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
/* THE PLACES OF A TEXT: a block's content, a cell's, a Text's literal --
 * bytes whose map places each of them in the source. An offset in a text
 * is NOT a column: `"> foo\nbar"` strips two bytes from the first line and
 * none from the second, and the two lines of one paragraph's content then
 * start at different distances from their line starts. The map knows
 * which; an element asks it here and never reads its runs. A map is live
 * for as long as the parse is: an element may ask while the block is open,
 * and the inline phase after every block has closed.
 *
 * Where the bytes `from` up to `to` of the text `map` places were written,
 * into `*place`: from where byte `from` begins to where byte `to - 1` ends,
 * or, when `to` is not past `from`, the empty place where byte `from`
 * begins. False, leaving `*place` as it is, when the map places no byte. */
bool markdown_core_text_place(markdown_core_parser *parser, const markdown_core_content_map *map, bufsize_t from,
                              bufsize_t to, markdown_core_place *place);
/* WHERE A READING'S FIELDS GO (core-element-decoupling.md 8.3). A reading
 * names a field of the node it makes as it reads it, by its own offsets,
 * and the engine places them in the source and keeps them on the node
 * (node.h, markdown_core_fields), which every query of the node's fields
 * and markers then reads: no grammar reads the node again. The offsets are
 * those of the text `map` places, or, with no map, those of the line in
 * hand `reader` reads, or, with neither, the source's own; `base` is added
 * to each. A target with no node or no parser adds nothing. */
struct markdown_core_reader;
typedef struct markdown_core_field_to {
    markdown_core_parser *parser;
    struct markdown_core_node *node;
    const markdown_core_content_map *map;
    const struct markdown_core_reader *reader;
    bufsize_t base;
    /* The inline run reading `map`'s text, whose frame places a field on
     * the run its cursor is on by arithmetic (inline_internal.h). */
    const struct markdown_core_inline_state *run;
} markdown_core_field_to;
/* Adds the bytes `from` up to `end` as `to`'s node's field of slot kind
 * `slot`, a markdown_core_slot_kind (NONE for a field no slot kind names).
 * An empty range adds nothing; with the parse failed when storage could
 * not be had. */
void markdown_core_field_add(const markdown_core_field_to *to, int slot, bufsize_t from, bufsize_t end);
/* A field a reading noted, to be added with the others it read at once
 * (markdown_core_field_many): its bytes and its slot kind, and, for one
 * with none, whether it is kept as the empty field a reading found nothing
 * written in (markdown_core_field_empty): `k=`'s value; and how it is
 * read (node.h, markdown_core_fields), a plain field when left 0, or an
 * anchor at `from` (markdown_core_field_put). */
typedef struct markdown_core_field_spot {
    bufsize_t from, end;
    int slot;
    bool empty;
    uint8_t how;
} markdown_core_field_spot;
/* Adds each of the `count` fields at `spots`, as markdown_core_field_add
 * would one by one. */
void markdown_core_field_many(const markdown_core_field_to *to, const markdown_core_field_spot *spots, size_t count);
/* The same, read as `how` says (node.h, markdown_core_fields): a field
 * lent to the node's holder, or where its literal starts; one `unique`
 * replaces the node's earlier field of its slot read so, as a reading of
 * the node again reads it again. */
void markdown_core_field_put(const markdown_core_field_to *to, int slot, int how, bufsize_t from, bufsize_t end,
                             bool unique);
/* A FIELD OF A CONSTRUCT THE PARSE DID NOT FINISH (node.h, OPEN), as the
 * reading that gave the construct up reports it: its slot kind, the kind of
 * the construct that owns it, and the offsets, in the reading's input, it
 * starts at and its reading stopped at. */
typedef struct markdown_core_field {
    markdown_core_slot_kind slot;
    markdown_core_node_kind owner;
    bufsize_t start, stop;
} markdown_core_field;
/* Adds the empty field of slot kind `slot` where byte `at` begins, which a
 * reading found nothing written in: `[t]()`'s destination. A query of the
 * node's fields leaves it out; a cursor there writes it. */
void markdown_core_field_empty(const markdown_core_field_to *to, int slot, bufsize_t at);
/* Keeps on `to`'s node a construct of kind `opens` the parse began and gave
 * up (node.h, OPENER and OPEN), NONE when its kind is still open: its
 * opening syntax, the bytes `from` up to `end`, read by a line of the block
 * phase when `line`, and the `count` fields at `fields` it wrote, each from
 * its start to where its reading stopped, an empty one at its start. */
void markdown_core_field_opener(const markdown_core_field_to *to, markdown_core_node_kind opens, bool line,
                                bufsize_t from, bufsize_t end, const markdown_core_field *fields, size_t count);
/* The input line, counted from 1, byte `at` of the text `map` places was
 * read from: 0 when the map places no byte. */
int markdown_core_text_line(markdown_core_parser *parser, const markdown_core_content_map *map, bufsize_t at);
/* The indentation of the line byte `at` of the text was read from, after
 * its container prefixes: 0 when the map places no byte. */
int markdown_core_text_indent(markdown_core_parser *parser, const markdown_core_content_map *map, bufsize_t at);
/* `*map` comes to place a slice of the text `owner` places, its bytes
 * `from` up to `from + length`, sharing the owner's marks: it allocates
 * nothing. A slice of no bytes, or of a text placed nowhere, places none:
 * 0 then, and 1 otherwise. */
int markdown_core_text_share(markdown_core_parser *parser, const markdown_core_content_map *owner,
                             markdown_core_content_map *map, bufsize_t from, bufsize_t length);
/* `*map`, the map of a text being written, places its bytes from `offset`
 * where `owner`'s bytes `from` up to `from + length` were written, copying
 * the marks: for a text built of slices with other bytes between them. 0,
 * with the parse failed, when storage could not be had. */
int markdown_core_text_append(markdown_core_parser *parser, const markdown_core_content_map *owner,
                              markdown_core_content_map *map, bufsize_t from, bufsize_t length, bufsize_t offset);

int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line,
                                      bufsize_t source);

/** Append a source run for content already assembled by a producer. Runs
 * must be contiguous in the parser vector and have increasing content offsets.
 * source_width is the authored width represented by each logical byte, and
 * source_step is the source-byte stride. Allocation failure marks the parse lost. */
int markdown_core_parser_append_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t offset,
                                             int line, bufsize_t source, int source_width, int source_step);
/** Project a logical inline range, including its Text literal mapping. */
void markdown_core_inline_state_place(markdown_core_inline_state *inline_state, markdown_core_node *node, int from,
                                      int to);

/** Return the source offset just after the previously processed line's last
 * byte, excluding its line ending: the line's start for an empty line. In a
 * table cell's content, which the parser reads as an input of its own, it is
 * the end of the cell line's last byte in the source line.
 */
bufsize_t markdown_core_parser_get_last_line_end(markdown_core_parser *parser);

/** Add a child to 'parent' during the parsing process.
 *
 * If 'parent' isn't the kind of node that can accept this child,
 * this function will back up till it hits a node that can, closing
 * blocks as appropriate.
 */
markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column);

/** Register 'element' after every element 'builder' already holds.
 *  See the documentation for markdown_core_element for more information.
 *
 *  Returns 'true' if the 'element' was registered, 'false' otherwise: on
 *  allocation failure, for a descriptor the registration rule refuses -- one
 *  that asks a step without a step, or a block kind as a scope kind, or one
 *  kind as both an exit and a scope kind, or only part of the document
 *  lifecycle,
 *  or a flanking-transparent byte outside ASCII -- and once the dialect holds
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
    /* The items the block's text opened with were all of it (THE FRONT
     * CUT): the block keeps its kind and yields no node of its own. */
    MARKDOWN_CORE_NODE_SET_KIND_CUT,
} markdown_core_node_set_kind_result;

/** Change 'node', which 'parent' holds, to the internal kind encoded by 'kind'.
 *
 * Return OK on success, REJECTED when 'parent' is NULL or its containment
 * disallows the change, or ALLOCATION_FAILED when replacement node data
 * cannot be allocated. Either failure preserves the original kind, data, and
 * children.
 *
 * A change releases values owned by the old kind and installs the new kind's
 * defaults. The node's address and element-owned opaque data are preserved;
 * its id is 0 again, since an id names a node while its kind holds.
 * A record that fits the node's existing cell needs no allocation.
 * Setting the current kind succeeds without allocating or changing its data.
 */
markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node *parent,
                                                               markdown_core_node_type kind);

/** Set the parser element responsible for creating 'node'.
 */
int markdown_core_node_set_element(markdown_core_node *node, const markdown_core_element *element);

/**
 * ## Inline parser element helpers
 *
 * The inline parsing process is described in detail at
 * <http://spec.commonmark.org/0.24/#phase-2-inline-structure>
 */

/** The offset in the content of the inline root being parsed of byte
 * `offset` of the chunk the given inline state reads: the same offset in the
 * root's own run, and in a field's the root's offset its map names. A
 * definition lookup names where it is with it
 * (markdown_core_parser_find_definition).
 */
uint32_t markdown_core_inline_state_root_offset(markdown_core_inline_state *inline_state, int offset);

/** Gets the markdown_core_chunk being operated on by the given inline state.
 */
struct markdown_core_chunk *markdown_core_inline_state_get_chunk(markdown_core_inline_state *inline_state);
#ifdef __cplusplus
}
#endif

#endif
