#ifndef MARKDOWN_CORE_ELEMENT_H
#define MARKDOWN_CORE_ELEMENT_H

#include "node_type.h"
#include "buffer.h"
#include "markdown-core-element-api.h"
#include "config.h"
#include "chunk.h"
#include "delimiter.h"
#include "node.h"
#include "attributes.h"
#include "measure.h"
#include "follow.h"
#include "tokenizer.h"
#include "compile.h"

struct markdown_core_block_start_context;
struct markdown_core_block_start;

typedef enum {
    MARKDOWN_CORE_INLINE_TOKEN = -1,
    MARKDOWN_CORE_INLINE_DEFAULT,
    MARKDOWN_CORE_INLINE_FALLBACK
} markdown_core_inline_precedence;

typedef enum {
    MARKDOWN_CORE_CONTENT_CONTAINER,
    MARKDOWN_CORE_CONTENT_PROSE,
    MARKDOWN_CORE_CONTENT_LITERAL
} markdown_core_content_mode;
/* Which element defines the structure of a node KIND -- distinct from
 * `node->element`, which is the element that created that node INSTANCE.
 *
 * This is the definition, a pure function of the kind. Sealing reads it once
 * per kind to project the dialect's kind record (dialect.h,
 * markdown_core_kind_record), and everything the engine asks of a node's kind
 * while parsing it asks that record: the structure's instance and the facts
 * of its descriptor, in one load. It is NOT cached on the node: a cached copy
 * is a second answer to "which element defines this kind" that has to be
 * rewritten at every kind change and can be wrong in between. */
extern const markdown_core_element *const markdown_core_block_structure[MARKDOWN_CORE_NODE_KIND_COUNT];
extern const markdown_core_element *const markdown_core_inline_structure[MARKDOWN_CORE_NODE_KIND_COUNT];

/* THE HOLDS: what a node of a kind may hold as a child, declared by the
 * kind's schema (node-types.json, `contain`) and resolved by its generator
 * into one bit per kind value of each class (`blocks`, `inlines`), so the
 * node tree tests a bit and names no kind (node.c,
 * markdown_core_node_can_contain_builtin). A ROOT kind is never a child, so
 * no mask holds it, and the dynamic policies (markdown_core_contain_ops
 * `allows`) are refused it too; an OWNED kind holds children only when its
 * node's own element declares containment. */
enum {
    MARKDOWN_CORE_HOLD_ROOT = 1u << 0,
    MARKDOWN_CORE_HOLD_OWNED = 1u << 1,
};

typedef struct markdown_core_hold {
    unsigned flags;
    uint32_t blocks, inlines;
} markdown_core_hold;

/* THE KIND TABLES -- the holds and the record ops below -- are indexed by
 * class then ordinal: a block kind at its ordinal, an inline kind at 32 past
 * it, and a value outside the table at the last index. No class numbers a
 * kind 0, and each class numbers fewer than 32 (the generator refuses more),
 * so every index that numbers no kind -- NONE's among them -- is an entry of
 * no kind: it holds nothing, is never a child and has no record. A mask, a
 * shift and one compare, with no branch on the class: the walks ask it of
 * every node they make, free and visit. */
#define MARKDOWN_CORE_KIND_TABLE_CLASS 32
#define MARKDOWN_CORE_KIND_TABLE_COUNT (2 * MARKDOWN_CORE_KIND_TABLE_CLASS)
/* The table bit of a kind's class: set for an inline kind, clear for a block. */
#define MARKDOWN_CORE_KIND_TABLE_BIT(kind) (((size_t)(kind) >> 9) & MARKDOWN_CORE_KIND_TABLE_CLASS)
/* A class that outgrows the tables, or a class bit the index does not read
 * where it is, is a compile error here. */
typedef char markdown_core_kind_table_fits[MARKDOWN_CORE_NODE_KIND_COUNT <= MARKDOWN_CORE_KIND_TABLE_CLASS ? 1 : -1];
typedef char markdown_core_kind_table_reads
    [MARKDOWN_CORE_KIND_TABLE_BIT(MARKDOWN_CORE_NODE_TYPE_INLINE) == MARKDOWN_CORE_KIND_TABLE_CLASS ? 1 : -1];
typedef char markdown_core_kind_table_blocks[MARKDOWN_CORE_KIND_TABLE_BIT(MARKDOWN_CORE_NODE_TYPE_BLOCK) == 0 ? 1 : -1];

static inline size_t markdown_core_kind_table_index(markdown_core_node_type kind) {
    const size_t ordinal = (size_t)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    return ordinal < MARKDOWN_CORE_NODE_KIND_COUNT ? (ordinal | MARKDOWN_CORE_KIND_TABLE_BIT(kind))
                                                   : MARKDOWN_CORE_KIND_TABLE_COUNT - 1;
}

extern const markdown_core_hold markdown_core_holds[MARKDOWN_CORE_KIND_TABLE_COUNT];

/* The holds of `kind`. */
static inline const markdown_core_hold *markdown_core_holds_for_kind(markdown_core_node_type kind) {
    return &markdown_core_holds[markdown_core_kind_table_index(kind)];
}

static inline const markdown_core_element *markdown_core_structure_for_kind(markdown_core_node_type kind) {
    unsigned index = (unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (index >= MARKDOWN_CORE_NODE_KIND_COUNT) {
        return NULL;
    }
    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind) ? markdown_core_inline_structure[index]
                                                  : markdown_core_block_structure[index];
}

/* How many elements one dialect holds at most. The block-start projection
 * lists a family's owners by byte -- a count and then owner indices -- so a
 * family may have at most 255 owners and an owner index at most 254; the
 * attachment API refuses the element that would break that, leaving the
 * dialect as it was, rather than the projection wrapping a byte. */
#define MARKDOWN_CORE_ELEMENT_LIMIT 255

/* THE RECORD: what the node walks (node.c) ask of a record a node holds --
 * its kind's record (`node->as`), whose ops its kind's schema names
 * (node-types.json `ops`, generated into markdown_core_records), and
 * an element's payload (`node->opaque`), whose ops its element names
 * (`payload`). ONE interface for both; each hook may be NULL, for no work
 * there. They are class methods: a node knows its kind and its element, not
 * the parse that made it.
 *
 * - `size`: the kind record's bytes, which the node's slot or its pool holds
 *   zeroed before `init`. A payload has none: its `init` makes it.
 * - `init`: a kind record's defaults; a payload's `init` makes `node->opaque`,
 *   or leaves it NULL when storage could not be had.
 * - `free`: releases what the record owns. Its node-valued fields were taken
 *   by the walk first (`visit`), and it must not destroy them.
 * - `viewed`: the record of an inline that views the content it was read
 *   from takes copies of those bytes. False when storage could not be had.
 * - `visit`: the node-valued fields the record holds, each an independent
 *   child-tree root and not a child edge, the non-NULL ones in source order.
 *   The visitor's slot is the field, in the record's storage, so a walk can
 *   pair it with the same field of another node's record. A payload's fields
 *   survive kind conversion with it. */
typedef struct markdown_core_record_ops {
    size_t size;
    void (*init)(markdown_core_node *node);
    void (*free)(markdown_core_node_pool *pool, markdown_core_node *node);
    bool (*viewed)(markdown_core_node *node);
    int (*visit)(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context);
} markdown_core_record_ops;

extern const markdown_core_record_ops *const markdown_core_records[MARKDOWN_CORE_KIND_TABLE_COUNT];

/* The record ops of `kind`: for a kind with no record, or a value outside
 * the kind table, the record of nothing, whose hooks are all NULL. */
static inline const markdown_core_record_ops *markdown_core_record_for_kind(markdown_core_node_type kind) {
    return markdown_core_records[markdown_core_kind_table_index(kind)];
}

/* THE PARSE'S STAGES, which any element may take part in (blocks.c): the
 * engine asks every element of the dialect that declares one, in
 * descriptor order. `prepare` comes before completion, `finish` at the
 * parse's end and at each edit's, once the tree is complete and before it
 * is published, when the parse has not failed; `release` lets what the
 * element keeps of the parse go -- at the parse's release, and at a flush
 * that is its last; an edit's `reopen`, after the document's, keeps what
 * starts before `start`, and its `splice`, before the document's, takes
 * back what the old text kept from `point` of the new one. A false
 * `reopen` or `splice` is storage that could not be had. */
typedef struct markdown_core_stage_ops {
    void (*prepare)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*finish)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*release)(const markdown_core_element_instance *);
    bool (*reopen)(const markdown_core_element_instance *, markdown_core_parser *, uint32_t);
    bool (*splice)(const markdown_core_element_instance *, markdown_core_parser *, uint32_t);
} markdown_core_stage_ops;

/* THE DOCUMENT'S LIFECYCLE: what the parse asks of the one element of a
 * dialect that owns the document, at each of its steps: `init` as the
 * parse opens, `read` before the first line, `publish` once the text is
 * read, and an edit's `reopen` and `splice` of the published tree. Every one is required: registration
 * refuses an owner that leaves out one. */
typedef struct markdown_core_document_ops {
    /* The kind of the root node the document is: the engine's root role
     * (the dialect's `root`), which it makes and never takes as a child. */
    markdown_core_node_type kind;
    void (*init)(const markdown_core_element_instance *, markdown_core_parser *);
    /* Reads what the document's input opens with, before its first line,
     * through the reader, which stands at the input's start with no line in
     * hand. It answers the bytes it took, whole lines through the last one's line
     * ending, or 0: the engine reads on at the line after them. */
    size_t (*read)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_reader *);
    /* The last step of the parse: the tree is final, and the owner publishes
     * it (ids, extents, definition tables). */
    void (*publish)(const markdown_core_element_instance *, markdown_core_parser *);
    /* An edit reopens the flushed document `root` (blocks.c, THE REOPEN):
     * its children from `child` on go, and every state the document keeps
     * of them -- what its publish matches against, its definitions -- stays
     * only for what starts before `start`. False when storage could not be
     * had. */
    bool (*reopen)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *, uint32_t,
                   uint32_t);
    /* The edit's reading takes the old children the reopen let go back
     * (blocks.c, THE SPLICE), from the one at `child` among them on, whose
     * line starts at `point` of the new text: the root holds them again, and
     * every state the document kept of what they hold -- their definitions,
     * their headings, measured from the text's end -- is back. False when
     * storage could not be had, with the children still the records'. */
    bool (*splice)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *, uint32_t,
                   uint32_t);
} markdown_core_document_ops;

/* The measure (measure.h) `*measure` with the measure an element takes of
 * the children of the root `root` from `from` up to `to` after its own:
 * false when storage could not be had. */
typedef bool (*markdown_core_measure_func)(const markdown_core_element_instance *, const markdown_core_node *root,
                                           uint32_t from, uint32_t to, markdown_core_measure *measure);

/* THE COMPLETION'S HOOKS: what completion's walk asks of an element --
 * `complete` of each node of its kinds, `reparse` of one completed again,
 * and its finish `step` at the events its kind lists name. Each may be NULL
 * (NULL lists: the step is asked nowhere). */
typedef struct markdown_core_finish_ops {
    /* The element's completion of a node, which completion's walk asks at the
     * ENTER of every node of a kind the element is the structure of, after
     * the node's inline content is parsed and before its explicit anchor is
     * reserved; `depth` is the word depth of the node's ancestors. Text turns
     * an escaped space inside a word into NBSP; a fenced code block whose
     * info string names a formula becomes a FormulaBlock. The hook may give
     * a node that holds no inline content and owns no fields another kind
     * that holds and owns none either. A failure fails the parse
     * (markdown_core_parser_fail), and the walk ends at its next check. */
    void (*complete)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *, int depth);
    /* A kind whose inline content is parsed before completion's walk
     * (`deferred`, by the document's preparation) parses one node's
     * again: a block completed again (blocks.c, A LOOKUP THAT FOUND NOTHING)
     * asks it once the node's inline content is released, and the walk then
     * completes the node as it completed it the first time. */
    void (*reparse)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *);
    /* The element's kinds parse their inline content before completion's
     * walk, by the document's preparation, and not as the walk enters them. */
    bool deferred;
    /* A node of the element's `deferred` kinds whose inline content parses
     * as a paragraph's does, in place from its seams when an edit's reading
     * leaps in it (blocks.c, THE SEAMS): one whose content declares nothing
     * the preparation orders. NULL: none does. */
    bool (*seams)(const markdown_core_element_instance *, const markdown_core_node *);
    /* The measure of the children of a root of the element's kinds that
     * parses from seams, which each seam keeps of the children before it,
     * so an edit's reading measures only the children it made (blocks.c,
     * THE SEAMS; markdown_core_parser_leapt). NULL: none. */
    markdown_core_measure_func measure;
    /* The element's finish step (the API header states the contract), asked
     * from inside completion's walk at the EXIT of every node whose kind is
     * in `exits` and at the ENTER and EXIT of every node whose
     * kind is in `scopes`. It costs the document nothing at any
     * other event: the engine projects the steps by (event, kind) once per
     * parse, so a step asked at Text is not so much as looked at when a
     * Paragraph exits, nor when a Text opens. */
    markdown_core_finish_step_func step;
    /* The node kinds the element's finish step ACTS ON, terminated by
     * MARKDOWN_CORE_NODE_NONE; NULL declares nothing. This is THE GATE: the
     * set is intersected with the kinds the parse has produced so far, read
     * at each event the step is asked at, and a step that cannot find
     * anything is skipped there. A step whose trigger kind is CREATED by an
     * earlier hook must therefore name that creator kind too.
     *
     * The kinds a step acts on are not always the kinds it is asked at, so a
     * step says where it is asked separately, below, and the gate stays what
     * makes a document with nothing for the step pay nothing at each event.
     *
     * The kinds are declared as KINDS rather than as a precomputed bit set so
     * that each one keeps its namespace: block and inline values collide once
     * masked, and a Formula written where a block kind belongs has to be
     * detectable rather than silently becoming some unrelated block's bit.
     * The engine projects them per parse. */
    const markdown_core_node_type *acts;
    /* WHERE A FINISH STEP IS ASKED, two lists terminated the same way; NULL
     * declares nothing.
     *
     * `exits`: the step receives the EXIT of these kinds, the
     * point where the node's subtree is complete and the node may be
     * rewritten. It names the kind of the node the step is handed -- LIST
     * for a list's layout, TEXT for autolink's scan.
     *
     * `scopes`: the inline kinds whose EXTENT the step tracks. It
     * receives their ENTER and their EXIT and nothing else about them: it acts
     * on nothing there, it learns that the nodes to come are inside one, and
     * it keeps that in its per-root state word. Autolink declares LINK -- an
     * address inside a Link is already a link's text.
     *
     * The two are disjoint: a kind in both would be one event asked twice,
     * and registration refuses the descriptor. It also refuses a step that
     * names no kind in either list, which would never be asked, a block kind
     * as a scope kind, whose extent completion never walks (the blocks below
     * a block complete before it), and a kind the dispatch table cannot index
     * (an ordinal at or past MARKDOWN_CORE_NODE_KIND_COUNT, or a value of
     * neither class). */
    const markdown_core_node_type *exits;
    const markdown_core_node_type *scopes;
} markdown_core_finish_ops;

/* THE FOLD: what a block of the element's takes from each child as the
 * child settles into it, so that what it reads as it closes or completes
 * needs no walk of its children. Registration requires `child` and `kinds`
 * together. */
typedef struct markdown_core_fold_ops {
    /* THE FOLD HOOK (docs/plans/2026-09-29-incremental-parsing.md, 5.6),
     * asked of a block of a kind in `kinds` as a child settles into it
     * (blocks.c, A BLOCK SETTLES), after the child's blank-line answer is
     * folded into a block whose answer is its children's: the block's flags
     * `flags` with what they take from a child whose flags are `child`, so
     * that what the block reads as it closes or completes is there without a
     * walk of its children. It writes nothing of the block but the values
     * the block publishes from the flags it returns: a commit writes the
     * result as the block's flags. */
    markdown_core_node_internal_flags (*child)(const markdown_core_element_instance *, markdown_core_node *,
                                               markdown_core_node_internal_flags, markdown_core_node_internal_flags);
    /* The block kinds the fold hook is asked at, kinds whose structure is
     * the element, terminated by MARKDOWN_CORE_NODE_NONE; an element with a
     * fold hook declares at least one, and one without declares NULL. */
    const markdown_core_node_type *kinds;
    /* The element's flags the fold hook writes, which a splice below a
     * reopened block folds again from all of its children (blocks.c, THE
     * NESTED SPLICE). */
    unsigned flags;
} markdown_core_fold_ops;

/* THE CONTAINMENT: what a node of the element's may hold as a child, which
 * the node tree asks of a node by its element (node.c,
 * markdown_core_node_can_contain_type). Class methods: a node knows its
 * element and not a parse. */
typedef struct markdown_core_contain_ops {
    /* A retained built-in payload may only parent these kinds after
     * conversion. NULL uses the core kind domain. */
    const markdown_core_node_type *kinds;
    /* The element's own decision for any child, in place of the core's
     * rule (a dynamic policy); NULL for the core's rule. */
    markdown_core_can_contain_func allows;
} markdown_core_contain_ops;

/* THE CONTENT: what a block of the element's holds -- blocks, prose or
 * literal lines (`mode`), whether its content is inline content
 * (`inlines`), and the per-node answers when its kind alone does not say
 * (`holds_inlines`, `holds_lines`, class methods). The dialect projects it
 * into each kind's record (dialect.h, the kind flags) as it is sealed. */
typedef struct markdown_core_content_ops {
    markdown_core_content_mode mode;
    bool inlines;
    /* The element's blocks are paragraphs, the prose blocks the engine's
     * paragraph rules apply to (IS_PARAGRAPH, dialect.h). */
    bool paragraph;
    markdown_core_contains_inlines_func holds_inlines;
    markdown_core_accepts_lines_func holds_lines;
} markdown_core_content_ops;

/* THE INLINE PARSE: what the inline engine asks of an element -- its
 * tokens (`match` at its `dispatch` bytes, `insert` for a delimiter pair of
 * its `rule`, `text` for the text between them), its run's record (`init`,
 * `finish`, `dispose`, `holds`, `report`), and, of the block whose inline
 * content runs (its structure), `enter`. Each may be NULL; the
 * dialect projects who answers what by byte and by hook as it is sealed. */
/* What a rule makes of a label at its closer (below). */
typedef enum { BRACKET_UNMATCHED, BRACKET_MATCHED, BRACKET_REJECTED } markdown_core_bracket_match;
/* A RULE BY WHICH AN ELEMENT CLOSES A LABEL at its element's closer byte
 * (label.h, markdown_core_inline_ops `closer`): the core's label end asks
 * the dialect's rules for that byte in ascending `precedence`, equal ones in
 * descriptor order. `end` answers MATCHED when it makes its construct,
 * REJECTED when no construct is made of the label at all, and UNMATCHED to
 * leave it to the next rule; with neither MATCHED, the closer is text. A
 * `mine` rule closes only the labels its element opened, and those close
 * only by its element's `mine` rules. */
struct markdown_core_opener;
struct markdown_core_label_made;
typedef struct markdown_core_close_rule {
    int precedence;
    bool mine;
    /* With the reading standing after the closer: MATCHED with what it
     * makes in `*made`, which it fills. It consumes nothing; what it read
     * past the closer is the end's (`past`). */
    markdown_core_bracket_match (*end)(const markdown_core_element_instance *, markdown_core_tokenizer *,
                                       struct markdown_core_opener *, struct markdown_core_label_made *made);
} markdown_core_close_rule;

/* A CONSTRUCT OF THE ELEMENT'S TEXT (plan symbols-cleanroom.md 23): the
 * construct, and the bytes it may start at, a NUL-terminated list without
 * NUL. The dialect copies it with its context set to the element's
 * instance, which its states reach through markdown_core_self. */
typedef struct markdown_core_text_construct {
    const char *bytes;
    markdown_core_construct construct;
} markdown_core_text_construct;

typedef struct markdown_core_inline_ops {
    /* THE ELEMENT'S TEXT (plan symbols-cleanroom.md 23): its constructs,
     * `construct_count` of them, tried at a byte in the dialect's order of
     * `precedence` and then of descriptors, and its token kinds,
     * `token_count` of them, numbered from the instance's `token_base` as
     * the dialect is sealed, with how compile makes each (`tokens`, one
     * entry per kind, its context set to the instance). */
    const markdown_core_text_construct *constructs;
    size_t construct_count;
    const markdown_core_compile_ops *tokens;
    size_t token_count;
    /* How compile makes the core's data, the text no construct read: the
     * dialect's last element that says. */
    const markdown_core_compile_ops *data;
    /* Negative/zero/positive precedence separates protected tokens, ordinary
     * alternatives, and literal fallbacks without a second dispatch algorithm.
     * A byte's owners are asked in ascending precedence -- any value, not only
     * the named ones -- and equal precedences in descriptor order. */
    markdown_core_inline_precedence precedence;
    /* The kind of the plain text nodes the core's data makes: the engine's
     * plain role (the dialect's `plain`), the one kind it merges, maps and
     * trims. */
    markdown_core_node_type plain;
    void (*init)(const markdown_core_element_instance *, markdown_core_inline_state *);
    void (*enter)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_inline_state *,
                  markdown_core_node *);
    void (*dispose)(const markdown_core_element_instance *, markdown_core_inline_state *);
    /* Keeps on the run's node what a label no closer met gives up (the
     * constructs it gave up, markdown_core_inline_open), as the core asks
     * it of each such label as the reading ends (label.h), innermost
     * first. */
    void (*left)(const markdown_core_element_instance *, markdown_core_tokenizer *, struct markdown_core_opener *);
    /* A label's body is decided, its construct made or not, before its
     * attention pairs (label.h, markdown_core_label_decide). */
    void (*decided)(const markdown_core_element_instance *, markdown_core_tokenizer *, struct markdown_core_opener *);
    /* The element's rules for closing a label, `close_count` of them, each
     * asked at `closer`, the byte they close a label at: the dialect reads
     * the label's end there (label.h). */
    const markdown_core_close_rule *closes;
    size_t close_count;
    unsigned char closer;
    /* Parsed delimiter semantics are declared by their element. Pairing,
     * run classification and node construction stay in the shared engine. */
    markdown_core_delimiter_rule rule;
    unsigned char character;
    delimiter_rule_spec delimiter;
    /* THE PAIRS OF A RULE WHOSE RUNS MAKE NO GROUP (attention.h; a rule of
     * no `minimum_width`): whether its opener and closer, sequence tokens,
     * make what the element makes of them, one token of its kind `cover`
     * over both and all between, with the word `*data`, where a pass of
     * `holder` puts it (markdown_core_attention_allows). */
    bool (*covers)(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                   markdown_core_node_type holder, const markdown_core_token *opener, const markdown_core_token *closer,
                   uint32_t *data);
    uint16_t cover;
    /* The bytes flanking looks through (attention.h), a NUL-terminated
     * list of ASCII bytes, or NULL. */
    const char *transparent;
} markdown_core_inline_ops;

/* THE OPENING: how a block of the element's opens on a line the open
 * containers left -- its starts (below), tried at the line's first
 * non-space byte, and `text` for the text block every dialect has. */
/* THE FRONT CUT (blocks.c): one item an element cuts off the front of a
 * block's text -- a link reference definition off a paragraph's. The
 * engine hands the element's item construct (markdown_core_open_ops `item`)
 * the text left, `rest`, `at` bytes into the block's text `text`, through
 * the turn (block_internal.h, markdown_core_block_turn `cut`). The
 * construct reads one item there, writes its `length` and says in `wide`
 * whether the bytes through the line read decided it rather than its own
 * and the one after them, and succeeds with a token whose handler makes
 * the item's node, `made`. With `measure` it only measures the item: it
 * writes the length and makes no token. The engine places and settles the node, notes
 * what the cut read, and moves the block's front past it. */
typedef struct markdown_core_cut {
    markdown_core_node *block, *parent, *made;
    markdown_core_chunk text, rest;
    bufsize_t at, length;
    bool wide, measure;
} markdown_core_cut;

/* A START OF THE ELEMENT'S BLOCKS (plan symbols-cleanroom.md 23.14): a
 * construct tried at the first non-space byte of what the open containers
 * left of a line, its state reaching the line through the turn
 * (block_internal.h, markdown_core_block_turn). It only reads: what it
 * found it writes in the turn's `start`, and it succeeds with one token of
 * the element's (markdown_core_block_found), whose compile handler opens
 * the block. The dialect lists the starts by the line's key -- the first
 * non-space byte, a blank line, an indented line -- tried in ascending
 * `precedence`, then descriptor order, then the element's own.
 *
 * `bytes` is the COMPLETE set of first bytes the start can succeed at, a
 * NUL-terminated list, or NULL for every line, blank ones too. A byte left
 * out does not make the parser faster; it makes the construct silently
 * never recognised, so api_test checks the set against the grammar. A set
 * speaks about ONE line that is not indented code: a grammar decided by a
 * later line cannot be expressed here. Four columns of indentation are a
 * start of their own, decided by no byte: an indented line is tried only
 * at the starts whose `indent`, the most indentation they open at, reaches
 * it. A `last` start is tried only where an ordinary paragraph would open
 * (the line neither lazy nor over a paragraph), and the line opens nothing
 * after it: the block it opens takes the line's text. A start with a
 * `within` kind is tried only where the line stands in an open block of
 * that kind (a table's row in its table): like `bytes`, a statement of the
 * grammar, never a filter for speed. */
typedef struct markdown_core_block_construct {
    const char *bytes;
    int indent;
    int precedence;
    bool last;
    markdown_core_node_type within;
    markdown_core_construct construct;
    /* Releases what the construct found and holds in the start's record
     * (`found`) when its token is not compiled: the parse failed after it
     * succeeded. NULL when its record holds nothing to release. */
    void (*drop)(const struct markdown_core_element_instance *self, struct markdown_core_block_start *start);
} markdown_core_block_construct;

typedef struct markdown_core_open_ops {
    const markdown_core_block_construct *starts;
    size_t start_count;
    /* The element's block token kinds, `token_count` of them, numbered
     * from the instance's `block_base` as the dialect is sealed, with how
     * compile makes each (one entry per kind, its context set to the
     * instance). */
    const markdown_core_compile_ops *tokens;
    size_t token_count;
    /* THE CONTENT (plan symbols-cleanroom.md 23.14, S3): the start tried
     * on a line nothing else opens on, which opens the text block that
     * takes the line, a paragraph. Its construct succeeds with a token of
     * the element's, as a start's does, and the handler opens the block.
     * The dialect's last element that has one owns it. */
    const markdown_core_block_construct *content;
    /* THE CONTENT'S ITEM (markdown_core_cut, THE FRONT CUT): the construct
     * tried at the front of what is left of a cutting block's text, `bytes`
     * the COMPLETE set of first bytes it can succeed at, NULL for every
     * byte, as a start's are. Projected into the dialect's cut owners. */
    const markdown_core_block_construct *item;
} markdown_core_open_ops;

/* THE LINES: how an open block of the element's takes each line after its
 * first -- its `continuation`, a lazy line (`lazy*`), its end condition
 * (`ends`), a blank line's mark (`blank*`) -- and how it closes (`close`).
 * Each may be NULL; the dialect projects the prefixes and the blank answers
 * into the kind records as it is sealed. */
struct markdown_core_line_ops {
    /* THE CONTINUATION (plan symbols-cleanroom.md 23.14, S2): the construct
     * tried at a line after the block's first, from the reader's offset,
     * where the blocks above it left the line, its state reaching the line
     * through the turn (block_internal.h, markdown_core_block_turn: `node`
     * the open block, `joining` the block a lookahead is about to add
     * below the open ones, NULL in the line's own reading). It fails when
     * the line does not continue the block, and succeeds when it does,
     * having moved the reader past the block's prefix -- with no token, or
     * with the core's verdict token (markdown_core_block_continued): the
     * line is the block's own closing line (MARKDOWN_CORE_TOKEN_CLOSE), a
     * closer the blocks inside decide (`_PEND`), or a line the block takes
     * whole, which the open block inside it takes as a line (`_TAKE`). A
     * container's continuation is also asked by a block-start lookahead
     * (`look`), where it must change nothing but the reader. NULL: every
     * line continues the block. */
    markdown_core_construct continuation;
    /* Whether the continuation reads the line beyond the block's prefix: a
     * line of the block's own may close it or stand outside it (a fence, a
     * paragraph's blank line), so no edit's splice reads past it as past a
     * prefix (blocks.c, THE NESTED SPLICE). */
    bool beyond;
    /* The bytes the continuation can strip from a line besides
     * indentation: the COMPLETE set, as a gate's is, and NULL when it strips
     * indentation only; it strips at most one of them from a line. A
     * block-start question asked of a LATER line from raw source (the
     * definition gate) reaches that line's first stripped byte by walking
     * over indentation and these, one for each container above the block
     * asking, so a byte left out here does not
     * make the parser slower, it makes it WRONG: the construct inside this
     * container is silently never recognised. Projected with the hooks into
     * the dialect's `container_prefix`. A declared byte that is also the marker
     * byte of the grammar asking (':' or '~' for a definition) cannot be told
     * from that marker in raw source, so the key hands such a line to the
     * lookahead rather than walking over it. */
    const char *prefix_bytes;
    /* Whether a blank line it took leaves its last line blank (BLANK_ASK,
     * dialect.h); NULL decides by whether it takes lines. */
    bool (*blank_last)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *,
                       markdown_core_reader *);
    /* What a blank line means inside it (BLANK_OPAQUE, BLANK_RUNS and
     * BLANK_PROPAGATES, dialect.h). */
    bool blank_opaque;
    bool blank_runs;
    bool blank_spreads;
    /* THE END CONDITION, checked on a line the block took as content, in
     * the document's reading (the turn's `node` the block): its success
     * closes the block there. Its start NULL for none. */
    markdown_core_construct ends;
    /* THE BLOCK'S EXIT (plan symbols-cleanroom.md 23.16.1, P2): the token
     * of the element's (one of its open ops' `tokens`) whose exit the core
     * compiles as a block of the element's closes inside the block that
     * holds it, which it settles into later, NULL for none. Its handler
     * reads the block through the turn (`node`, `*container` the holder,
     * `reader`) -- its content, its children and its open state -- and
     * makes the node the block settles as from them, never writing them:
     * what it makes of the content is a view of it, a literal or the cuts of
     * its text. What the block yields to its holder is settled into the
     * holder before it or written on the holder (a block identifier on a
     * list item's first line). The token covers no bytes: the block's are
     * read. */
    const struct markdown_core_compile_ops *shut;
    /* Its blocks' text may open with items the dialect's elements cut off
     * its front (blocks.c, THE FRONT CUT), which the engine cuts as a block
     * closes or changes kind; a block whose text was all such items yields
     * no node of its own, and closes as the line that starts an old item of
     * a kind the element joins is read when its items were all decided
     * whole. */
    bool cuts;
    /* A block of a cutting kind that changes to this kind keeps its text
     * as written: the engine cuts nothing first (a table whose header row
     * was a paragraph's text). */
    bool uncut;
    /* Its continuation may answer MARKDOWN_CORE_TOKEN_PEND: a closer whose
     * ownership the blocks inside decide, which a line the blocks inside
     * continue settles for them. */
    bool pending;
    /* A lazy line -- one that did not match every open container's prefix
     * and opened no block -- is offered to the current block, and `lazy`, a
     * check in the document's reading (the turn's `node` the block), says
     * whether it takes the line, by succeeding: a block of text adds it to
     * its text, any other opens the content the line starts in it (THE
     * CONTENT). A block that accepts the line takes it as a paragraph takes
     * a lazy line: the starts that refuse a lazy line (`block_start_context`)
     * open no block on it, so the line is lazy exactly when it would be
     * after a paragraph's line. Its start NULL for none. */
    markdown_core_construct lazy;
    /* THE TEXT OF ITS LINE: a construct attempted on the line's text from
     * the reader's offset, at the line's TEXT step (blocks.c), in the
     * document's reading, the turn's `node` a block the line opened. It
     * succeeds when the block takes the text, its tokens, compiled by the
     * element's block token handlers (markdown_core_open_ops `tokens`),
     * making a field of its own of it -- a callout's title; it fails when
     * the block takes none, and a paragraph does. What the block reads of
     * the text is no step of the line's: the line gave the text away there
     * (blocks.c, A REST INSIDE A LINE), as it gives a paragraph its text. A
     * block of the element that a rest inside that line reopens is given
     * the text again, and makes the field anew. Its start NULL for none. */
    markdown_core_construct text;
    /* The flags of the block that its container's prefix match sets line by
     * line, which a lookahead puts back. A line that holds a byte other
     * than spaces, tabs and container prefix bytes sets them as no line
     * before it matters, which THE LEAP (blocks.c) relies on. */
    unsigned speculative;
};

/* THE ELEMENT'S EDIT OPERATIONS: what an edit's reading asks of an element
 * (blocks.c, THE REOPEN and THE SPLICE), and what a session's reading notes
 * for the edits to come (`stops`). The dialect folds `awaits` into kind
 * flags as it is built, so no reading of a stream that is
 * not a session's looks here. */
typedef struct markdown_core_edit_ops {
    /* An edit may reopen a closed block of the element's at one of its
     * children and read on in it (blocks.c, THE NESTED REOPEN): the block
     * holds blocks, and its builder state at the start of a child's line is
     * its open state, the folds of the children before it, and what
     * `reopen_block` gives back. */
    bool reopens;
    /* The builder state of `node`, reopened at the start of a child's line,
     * that is neither a fold nor the engine's: the element's flags as a
     * fresh parse holds them there, the line before blank past the
     * containers' prefixes when `blank` says so, or what its children before
     * counted, as `next`, the old child the reading begins at when the block
     * is the chain's deepest, holds it (NULL otherwise). It runs before the
     * children before the line are folded into the block again. A block of
     * literal lines reopens at a later line of its own (blocks.c, THE
     * LEAP), with no children and `next` NULL. NULL when the element keeps
     * no such state. */
    void (*reopen_block)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *,
                         const markdown_core_node *next, bool blank);
    /* Whether the reading of `node`, at the start of the line of its old
     * child `next`, stands in the element's own state as the old reading
     * did there, as the last test before the reading takes `next` and the
     * old children after it back (blocks.c, THE NESTED SPLICE). NULL when
     * what `reopen_block` gives back is all of it. */
    bool (*resumes)(const markdown_core_element_instance *, const markdown_core_parser *,
                    const markdown_core_node *node, const markdown_core_node *next);
    /* Whether `open`, a block of the element that holds no open block,
     * closes as the line that starts an old sibling of kind `kind` is read,
     * as the old reading closed it there, whatever the line's prefixes
     * carry -- a definition, as the next one's term opens, or a paragraph of
     * link reference definitions, which the next one goes on (blocks.c, THE
     * SPLICE, THE NESTED SPLICE). NULL when only the prefixes close one. */
    bool (*yields)(const markdown_core_element_instance *, markdown_core_parser *, const markdown_core_node *open,
                   markdown_core_node_type kind);
    /* The block's first child is read with the block's opening and never
     * as a later child: no reopen rests at it, though the children abut (a
     * table's head row). */
    bool headed;
    /* Where `node` holds the field `text` (markdown_core_line_ops) made of
     * the text of its first line, or holds NULL when it made none: a rest
     * inside the line may stand where that text starts (blocks.c, A REST
     * INSIDE A LINE), and the engine lets an old field go before the block
     * is given the text again (blocks.c, THE TEXT OF ITS LINE), so the
     * element only makes it. NULL for an element that has no `text`. */
    markdown_core_node **(*text)(markdown_core_node *node);
    /* The block kinds of the element whose children each start a line of
     * their own with a marker that no line of the child before takes, not
     * even lazily -- a list's items: a reopen may rest at one on the line
     * after the child before ends, blank or not, and an edit's reading take
     * one back there (blocks.c, THE NESTED REOPEN), terminated by
     * MARKDOWN_CORE_NODE_NONE, or NULL. */
    const markdown_core_node_type *abuts;
    /* The block kinds of the element a block after it, past blank lines,
     * may still go on in as its line is read -- a definition term joins the
     * definition list before it: below the root, where no close is noted
     * (blocks.c, THE REACH), a reopen does not rest at the child after one
     * (THE NESTED REOPEN), terminated by MARKDOWN_CORE_NODE_NONE, or NULL. */
    const markdown_core_node_type *awaits;
    /* The block kinds the element's block yields as it closes, more than
     * one, one after another on the lines that follow -- a paragraph's link
     * reference definitions: two of them on adjacent lines were one block
     * as the later one's line was read, terminated by
     * MARKDOWN_CORE_NODE_NONE, or NULL. */
    const markdown_core_node_type *joins;
    /* THE STOP LINES (blocks.c, AN OPENER READ TO THE END): a test of one
     * line of the text as written, without its line ending, true for every
     * line the element's reading from an opener in the document's root
     * could stop at -- its closer -- and for others as it likes. A reading
     * that read to the text's end and found none decides the same while no
     * line after its opener is one, whatever the lines are: it notes its
     * opener (markdown_core_parser_reach_open), and not what it read. NULL
     * for an element whose readings note what they read. */
    bool (*stops)(const markdown_core_element_instance *, const unsigned char *line, size_t length);
    /* A reading from an opener in the document's root that found its stop
     * line, the closer of the block it opens, decided only that it found
     * one: the lines before it are the block's whatever they are, and the
     * block reads them to that line as the reading did. It notes neither
     * what it read nor its opener (blocks.c, AN OPENER READ TO THE END),
     * and an edit of a later line of the block than its first leaps in it
     * (THE LEAP) unless the edit writes its last, the closer (session.c,
     * session_shut). */
    bool shuts;
    /* A block of literal lines an edit's reading closes as its old reading
     * did, at an old line of its own (blocks.c, THE SPLICE IN A BLOCK): it
     * takes its old flags back, and the element its own state at that
     * close, which `reopen_block` gave up. NULL when it keeps none. */
    void (*spliced)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *);
} markdown_core_edit_ops;

/* A FOLLOWER (docs/plans followers.md): a grammar written after a node
 * another element made -- its leader -- that makes no node of its own but
 * updates the leader: writes its information (attributes, an anchor, a
 * level) or changes its kind (a paragraph a setext underline makes a
 * heading, a code block a formula fence). The element that may follow
 * leaders of the kinds in `kinds` (ended by MARKDOWN_CORE_NODE_NONE) at
 * time `at` says so here; the engine keeps the leaders and asks it there,
 * and does what it asks of the tree (markdown_core_parser_set_node_kind, a
 * block's cuts), keeping the books. A kind and a time is followed by one
 * element, the last that declares it, but at MARKDOWN_CORE_FOLLOW_LINE,
 * where the engine asks no one: a follower there finds its leader where its
 * own line reads (the paragraph before an underline, the block before an
 * identifier line), and its declaration says what it does to it.
 *
 * What a leader is, is the leader's grammar's: an inline construct reports
 * the node a later token may follow (markdown_core_text_made), and a block's
 * element offers the end of a line of its own (markdown_core_parser_offer).
 * The times are markdown_core_follow_at (follow.h). */

/* What the engine hands a follower of its leader. */
typedef struct markdown_core_lead {
    markdown_core_node *node, *parent;
    /* MARKDOWN_CORE_FOLLOW_TAIL: and where the fields a follower reads in
     * the line go (markdown_core_field_to). */
    const unsigned char *line;
    bufsize_t from, to;
    bool live;
    markdown_core_field_to fields;
} markdown_core_lead;

typedef struct markdown_core_follow {
    markdown_core_follow_at at;
    const markdown_core_node_type *kinds;
    bufsize_t (*follow)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_lead *);
    /* A LINE AFTER A CLOSED LEADER: `closed` declares, of a follower at
     * MARKDOWN_CORE_FOLLOW_LINE, that its line may write a leader that
     * closed before it -- a block identifier its anchor -- so an edit's
     * reading takes no old blocks back after one (blocks.c, THE SPLICE). */
    bool closed;
    /* Of such a follower, whether a line, as written and without its line
     * ending, may be one that writes such a leader: an edit that writes no
     * such line, nor replaces one, leaves the leader before it as it was
     * (index.h, THE REST). NULL when any line may. */
    bool (*writes)(const markdown_core_element_instance *, const unsigned char *line, size_t length);
    /* A TAIL CUT FROM ITS LEADER'S TEXT THAT THE LEADER'S INLINES MAY GIVE
     * BACK: `yields` declares that a token of the leader's inline content
     * that crosses into the bytes the follower took from the back of its
     * text (node.h, `cut_back`) wins over it. Their run then reads the
     * whole text, its text ending where the cut begins, a token that starts
     * there ends the run, and one that crosses it has the engine ask `undo`,
     * which forgets what `follow` wrote and leaves no cut. */
    bool yields;
    void (*undo)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *);
    /* THE FORMER KIND: a follower that changes its leader's kind to `makes`
     * says what an edit's reading asks of the block it made (blocks.c, THE
     * LEAP, THE SPLICE IN A BLOCK). `former` names the kind the block read
     * its lines as -- the leader's, or NONE for a block of `makes` no
     * follower made -- which an edit's reading reopens it as at a later line
     * of its own; `revert` gives `node`, in `parent`, that kind back, with
     * the state its opening set, and in `*kept` what `redo` reads of the
     * block it was (false, the node as it was, when storage could not be
     * had); `redo` makes it the follower's again from what `revert` kept,
     * as the edit's reading takes back the old line that made it so, NULL
     * when the block's close or completion makes it so again. */
    markdown_core_node_type makes;
    markdown_core_node_type (*former)(const markdown_core_element_instance *, const markdown_core_node *);
    bool (*revert)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *,
                   markdown_core_node *parent, uint32_t *kept);
    bool (*redo)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *,
                 markdown_core_node *parent, uint32_t kept);
} markdown_core_follow;

/* THE FACET OF A KIND: what a published node of `kind` shows of itself,
 * which the public node readers ask of the kind by the element that
 * declares it -- for a payload of the element's own, its literal and where
 * its published text starts in it, a formula's placement, a directive's
 * name and where its label is, and the name the names service computes
 * for it. Each may be NULL, for what the kind does
 * not show or shows from the node itself. A list of them ends at
 * MARKDOWN_CORE_KIND_NONE. */
/* WHAT A NODE READS AS IN A NAME its text makes (names.h, a heading's
 * anchor), which a facet's `words` gives piece by piece, in order: `text`
 * takes bytes, `node` a node whose own reading goes there. Each answers
 * false when storage could not be had. */
typedef struct markdown_core_words {
    void *context;
    bool (*text)(void *context, const unsigned char *data, bufsize_t size);
    bool (*node)(void *context, const markdown_core_node *node);
} markdown_core_words;

typedef struct markdown_core_facet {
    markdown_core_node_kind kind;
    /* Whether a node of the kind is one field of slot kind NONE, all of its
     * own source: a text, an HTML tag or block, which hold no field of
     * their own and no marker. */
    bool whole;
    markdown_core_chunk *(*literal)(const markdown_core_node *, bufsize_t *start);
    markdown_core_placement (*placement)(const markdown_core_node *);
    /* The name the node holds, which may hold any byte, or NULL for none. */
    const markdown_core_chunk *(*name)(const markdown_core_node *);
    markdown_core_node **(*label)(const markdown_core_node *);
    /* Where a node of the kind keeps the name the names service computes
     * for it (node.h, A NODE'S NAME; names.h), when the element names it. */
    struct markdown_core_name *(*named)(const markdown_core_node *);
    /* What a node of the kind reads as in a name: its literal or its
     * children in turn, which the name reads itself, or, when `words` is
     * set, what that says (markdown_core_words); nothing by default, an HTML
     * tag's or a note's. */
    uint8_t reads;
    bool (*words)(const markdown_core_node *, const markdown_core_words *);
} markdown_core_facet;

enum { MARKDOWN_CORE_READS_NOTHING = 0, MARKDOWN_CORE_READS_LITERAL, MARKDOWN_CORE_READS_CHILDREN };

/* The words of a node whose children read in turn (an emphasis, a link). */
static inline bool markdown_core_words_children(const markdown_core_node *node, const markdown_core_words *words) {
    for (uint32_t i = 0; i < node->child_count; i++) {
        if (!words->node(words->context, node->children[i])) {
            return false;
        }
    }
    return true;
}

/* The words of a node whose literal reads as itself (a text, a code span). */
static inline bool markdown_core_words_literal(const markdown_core_node *node, const markdown_core_words *words) {
    return words->text(words->context, node->as.literal->data, node->as.literal->len);
}

/* A LABEL SPACE (labels.h): a lookup space the labels an element declares
 * are filed in and looked up from. A space is its `name`: every element
 * that declares or looks up labels in it names it in `spaces`, each with
 * its own descriptor, and registration refuses two that name one space
 * with different `folds`. The dialect numbers each space once, after the engine's own
 * ANCHOR, in descriptor order of first naming, and the instance holds the
 * numbers in `spaces`, in its descriptor's order. A space that `folds`
 * holds its labels in the reference-label normal form, any other their
 * bytes as written. */
typedef struct markdown_core_space {
    const char *name;
    bool folds;
} markdown_core_space;

/* The most spaces a dialect numbers: a kind is a key's first byte, and the
 * engine's ANCHOR and no kind's 0 take two of its values. */
#define MARKDOWN_CORE_SPACE_LIMIT 254

/* A SERVICE: what an element offers the others under a name, an
 * operations table of the interface the name stands for
 * (elements/services.h), which takes the provider's instance as its first
 * argument. A list of them ends at one with no `name`. The engine reads the
 * name alone: what the table does is the interface's and its provider's. */
typedef struct markdown_core_service {
    const char *name;
    const void *ops;
} markdown_core_service;

struct markdown_core_element {
    /* WHO THE ELEMENT IS AND WHAT IT STORES. */
    const char *name;
    /* The services the element offers (markdown_core_service), or NULL, and
     * the ones its code asks, by name, NULL-terminated
     * (markdown-core-element-api.h, "AN ELEMENT AS ONE PARSE HOLDS IT").
     * Sealing resolves each it asks, in this order, into the instance's
     * `needs`. No element names another: one depends on a service, which
     * any element may provide. */
    const markdown_core_service *provides;
    const char *const *needs;
    /* The label spaces the element declares or looks up labels in,
     * NULL-terminated (markdown_core_space); a space is its name, so every
     * element naming one names the same space. */
    const markdown_core_space *const *spaces;
    /* The bytes of the element's parse record and of its record in each
     * inline run (markdown-core-element-api.h, "AN ELEMENT AS ONE PARSE HOLDS
     * IT"); zero declares none. */
    size_t state_size, run_state_size;
    /* The nodes of other elements it follows (markdown_core_follow), ended
     * by one with no `kinds`, or NULL. */
    const markdown_core_follow *follows;
    /* The facets of the kinds it shows (markdown_core_facet), or NULL. */
    const markdown_core_facet *facets;
    /* Frees what the parse record holds as its parser is freed, or NULL
     * when it holds nothing of its own. */
    void (*dispose_parser)(const markdown_core_element_instance *, markdown_core_parser *);
    /* The parse's stages it takes part in (markdown_core_stage_ops), or
     * NULL. */
    const markdown_core_stage_ops *stages;
    /* What the element tells the names service of the nodes it asks names
     * for (names.h, markdown_core_name_ops), or NULL when it asks none. */
    const struct markdown_core_name_ops *names;
    /* How the names service spells every name (names.h,
     * markdown_core_name_spelling), or NULL: the dialect's last element
     * that declares one, its anchor element, spells them. */
    const struct markdown_core_name_spelling *spelling;

    /* THE INTERFACES THE ELEMENT IMPLEMENTS, one table for each algorithm
     * that asks it, in the order a line meets them. A NULL table says the
     * element takes no part in that algorithm; a NULL entry in a table says
     * the default its comment gives. */
    /* How a block of the element's opens (markdown_core_open_ops), or NULL
     * when none does. */
    const markdown_core_open_ops *opens;
    /* How an open block of the element's takes its lines and closes
     * (markdown_core_line_ops), or NULL; the instance holds the empty
     * table then. */
    const markdown_core_line_ops *lines;
    /* What a block of the element's holds (markdown_core_content_ops), or
     * NULL for a container of blocks. */
    const markdown_core_content_ops *content;
    /* What the element's nodes may hold as children (markdown_core_contain_ops),
     * or NULL for the core's rule of their kinds. */
    const markdown_core_contain_ops *contain;
    /* What a block of the element's takes from its children as they settle
     * (markdown_core_fold_ops), or NULL. */
    const markdown_core_fold_ops *fold;
    /* What the inline parse asks of the element (markdown_core_inline_ops),
     * or NULL when it takes no part in it. */
    const markdown_core_inline_ops *inlines;
    /* What completion's walk asks of the element (markdown_core_finish_ops),
     * or NULL. */
    const markdown_core_finish_ops *finish;
    /* The document's lifecycle (markdown_core_document_ops), for the one
     * element of a dialect that owns it, or NULL. */
    const markdown_core_document_ops *document;
    /* The element's payload on its nodes (`node->opaque`,
     * markdown_core_record_ops), or NULL when its nodes hold none. */
    const markdown_core_record_ops *payload;
    /* What an edit's reading asks of the element (markdown_core_edit_ops),
     * or NULL when it asks nothing. */
    const markdown_core_edit_ops *edit;
};

/* The element's containment kinds and its own decision for a node of its,
 * or NULL (markdown_core_contain_ops). */
static inline const markdown_core_node_type *markdown_core_node_contain_kinds(const markdown_core_node *node) {
    return node->element && node->element->contain ? node->element->contain->kinds : NULL;
}

static inline markdown_core_can_contain_func markdown_core_node_allows(const markdown_core_node *node) {
    return node->element && node->element->contain ? node->element->contain->allows : NULL;
}

/* THE FIELD ROOTS OF A NODE: the node-valued fields its kind's record and
 * its element's payload hold (markdown_core_record_ops, `visit`), the
 * kind's first, each in source order. The answer is no for almost every
 * node, so the kind's half is asked through its projected record
 * (dialect.h, MARKDOWN_CORE_KIND_FIELDS) -- one load and one flag -- and
 * the element's half through a NULL test. The root's record holds the
 * document's own state (its metadata), not inline roots, so the dialect
 * never marks the root kind (dialect.c). */
static inline int markdown_core_visit_kind_fields(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor,
                                                  void *context) {
    return markdown_core_record_for_kind((markdown_core_node_type)node->kind)->visit(node, visitor, context);
}

/* The subtrees the element of `node` owns through its payload. */
static inline int markdown_core_visit_element_subtrees(markdown_core_node *node,
                                                       markdown_core_owned_subtree_visitor visitor, void *context) {
    const markdown_core_element *element = node->element;
    return !element || !element->payload || !element->payload->visit || element->payload->visit(node, visitor, context);
}

#endif
