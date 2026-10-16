#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <assert.h>
#include <stdint.h>
#include "registry.h"
#include "node.h"
#include "buffer.h"
#include "dialect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A fatal parse transaction preserves its first cause. Optional grammar
 * rejection is a normal no-match; rejection after consuming a token is fatal
 * but is not an allocation failure. Buffers retain their own allocation flag. */
typedef enum {
    MARKDOWN_CORE_PARSE_OK,
    MARKDOWN_CORE_PARSE_ALLOCATION_FAILED,
    MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED
} markdown_core_parse_error;

#define MAX_LINK_LABEL_LENGTH 1000

/* WHAT A PARSE CONTINUES: the storage it takes nodes from and the tree it
 * continues.
 *
 * `previous` is the root of a tree a parse published, and the text it was
 * parsed from begins the text this parse reads: appends add to the end of a
 * text, so every byte of the old text keeps its offset. The published tree
 * continues it (docs/plans/2026-09-29-incremental-parsing.md, 5.9): every
 * node matched to an old node takes its id, every other node takes the next
 * id after `last_id`, and a matched node equal to its old node as a value is
 * that old node. On success the parse owns `previous`: it is the returned
 * root, or it is released into `pool` with every other node it retires, and
 * `last_id` is the last id issued. `node_count` is the number of nodes in
 * `previous`, and on success the number in the published tree. A fresh parse
 * continues nothing: `previous` is NULL and `last_id` is 0, so its root is 1
 * and every node is numbered in canonical walk order.
 *
 * `pool` lends the parse every node and resource slot it takes (node.h); it
 * outlives the parse, and its owner disposes it. */
typedef struct markdown_core_revision {
    markdown_core_node_pool *pool;
    markdown_core_node *previous;
    uint64_t last_id;
    size_t node_count;
} markdown_core_revision;

/* Immutable runs map logical content bytes to authored byte intervals.
 * Blocks append runs as lines arrive; transformed cells and decoded inline
 * tokens append runs when assembled. Nodes retain index slices with an origin,
 * so splitting a mapped Text shares its runs without copying a suffix. Text
 * consolidation appends each contributing run once to one contiguous map.
 * The parser owns all runs until the parse transaction ends. */
typedef struct {
    /* Logical offset at which this run begins. */
    bufsize_t content_offset;
    /* The source line the slice was copied from, counted from 1. */
    int line;
    /* The byte offset in the document source the run begins at. */
    bufsize_t source;
    /* Authored byte width represented by each logical byte in this run. */
    int source_width;
    /* Source bytes advanced per logical byte: one for copied bytes,
     * two for a contracted pipe escape, zero within a decoded token. */
    int source_step;
    /* Virtual indentation after container prefixes, before block content was
     * stripped. Slices retain this line provenance, including table leads;
     * synthetic inline runs use zero because they never close as blocks. */
    int indent;
} markdown_core_line_mark;

/* THE IDENTITY RUN, the first of every parse's content map: content offset
 * `o` at `o`, one byte wide. An inline root's parse places its nodes on it,
 * so their places are offsets in the root's content, and the maps of the
 * Texts and fields it makes are slices of that content
 * (markdown_core_inline_start_inlines). */
#define MARKDOWN_CORE_IDENTITY_MARK 0

/* A content span resolved against one node's map: where it begins, where it
 * ends, and the two runs that answer both.
 *
 * Placing an inline node and slicing its owner's map for it are the SAME two
 * queries -- `content_place(from)` and `content_end_place(to)` locate exactly
 * the runs `adopt_content_marks(from, to - from + 1)` then searched for again.
 * Every inline node paid for four binary searches to ask two questions.
 *
 * `has_end` is separate from `has_start` because the two offsets are checked
 * independently: several element entry points compute `to` as `x - 1`, which
 * is -1 at the start of a buffer, and a span may legitimately have a start and
 * no end. `last` is resolved by its own search rather than by walking forward
 * from `first`, so a span whose `to` precedes its `from` still names the run
 * that contains `to`. */
typedef struct {
    int first, last;
    /* Source byte of `from`, and the exclusive source end of `to`. */
    bufsize_t start, end;
    bool has_start, has_end;
} markdown_core_content_span;

/* Sequential source-order operations share scratch, such as an element's
 * deferred registrations or a table's regions. Space depends on entries, never on the
 * area of a sparse table or the numeric range of source coordinates. */
typedef struct {
    uint64_t *keys;
    unsigned char *entries;
    size_t key_capacity, entry_capacity;
    /* Key entries inspected by ordering, accumulated once per scan/pass. */
    size_t work;
} markdown_core_source_order;

/* A BLOCK INPUT still to read: a table cell whose content is blocks
 * (markdown_core_parser_queue_block_input), and the kinds of the blocks open
 * around it as it was queued, in its input and in every input around that,
 * as block kind bits. A cell's blocks are read after the document's, when
 * nothing that holds the cell is open, and these are the blocks its content
 * is inside. */
typedef struct markdown_core_block_input {
    struct markdown_core_node *owner;
    uint32_t enclosing;
} markdown_core_block_input;

/* AN INPUT the block parser reads: its bytes, and the geometry and grammar
 * facts of its lines. The driver and lookahead extend one index; a source
 * byte is scanned for line geometry once, whether the input is the
 * document or a mapped cell. A cell's content is read as an input of its
 * own, beside the document's, which keeps its index (blocks.c,
 * S_parse_block_inputs). */
typedef struct markdown_core_parser_input {
    const unsigned char *source;
    size_t size, scanned;
    /* Where the input's last complete line ends: a line ending with `\n`,
     * or with `\r` and another byte after it. A reading at or past it reads
     * the end of the input (markdown_core_parser_input_ends). A cell's
     * content is complete when it is read, so this is its size. */
    size_t complete;
    struct markdown_core_input_line *lines;
    struct markdown_core_normalized_line *normalized;
    struct markdown_core_line_facts *facts;
    size_t fact_count, fact_capacity;
    size_t line_count, line_capacity;
    /* Whether a column of the input may stand elsewhere than the same
     * offset from its line's start in the source: true for a cell's content,
     * and for the document once its scan finds NUL, which the block parser
     * reads as the three bytes of U+FFFD. */
    bool mapped;
    int first_line;
    /* The input's last line as the block parser will see it, normalized once
     * and reused by every lookahead that reaches it: it has no terminator of
     * its own in the source, and a line handed to the prefix matchers must
     * end in one. */
    markdown_core_strbuf last_line;
    bool last_line_ready;
} markdown_core_parser_input;

/* A definition lookup a snapshot's completion found nothing for
 * (markdown_core_parser_find_definition): the block being completed, the
 * block it settled into, and the lookup's kind and label, as written, at
 * `label_start` in the parser's `miss_labels`. */
typedef struct {
    struct markdown_core_node *block, *holder;
    markdown_core_registry_kind kind;
    bufsize_t label_start, label_length;
} markdown_core_definition_miss;

struct markdown_core_parser {
    /* Every definition and explicit anchor the parse declared (registry.h):
     * the block phase declares definitions as it reads them, the headings
     * declare their labels before inline content is parsed, and completion
     * reserves explicit anchors where it meets them. */
    markdown_core_registry registry;
    /* Whether completion runs for a snapshot rather than for the finalize
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.4): a lookup sees only
     * the definitions that start before the block being completed, and one
     * that finds none is recorded (markdown_core_parser_find_definition). */
    bool snapshot;
    /* The block being completed: the queued block the completion walk is in
     * (blocks.c, COMPLETION), or the heading whose inline content the
     * document's preparation parses. */
    struct markdown_core_node *completing;
    /* THE LOOKUPS A SNAPSHOT'S COMPLETION FOUND NOTHING FOR, in the order
     * they were made (plan 5.4). The finalize asks each again with every
     * definition visible, and completes again each block whose answer is one
     * now (blocks.c, A LOOKUP THAT FOUND NOTHING). */
    markdown_core_definition_miss *misses;
    size_t miss_count, miss_capacity;
    markdown_core_strbuf miss_labels;
    markdown_core_source_order source_order;
    /* The stack the finish stage's tree walks borrow in turn, the finish
     * walk's frames and then publishing's: each grows it to what it needs
     * (markdown_core_parser_walk_stack) and leaves it to the next, and the
     * stage releases it when it ends. */
    void *walk_stack;
    size_t walk_stack_size;
    /* THE COMPLETION QUEUE: every block of the tree, in the order it settled
     * (blocks.c, A BLOCK SETTLES), from `completion_first` to
     * `completion_last`, each linked through its scaffold to the block after
     * it and to the block it settled into (node.h, THE SCAFFOLD). A block
     * joins once its block structure is final; completing it parses its own
     * inline content and asks the finish steps at the events of its inline
     * nodes and at its own EXIT, with the block it settled into, after the
     * block phase, in queue order (S_finish_parse). Every block joins on its
     * own, so a block's completion never reaches another block. */
    struct markdown_core_node *completion_first, *completion_last;
    /* Run records released by finished runs, kept for the next run
     * (markdown_core_inline_state_from_buf); linked through their first word
     * and released with the parser. Each is the dialect's `run_state_size`. */
    struct markdown_core_inline_record *free_inline_records;
    /* The sealed dialect this instance parses with (dialect.h), which
     * shares the instance's allocation (blocks.c, markdown_core_instance),
     * and the context its setup was given. The context is the caller's; the
     * engine only carries it to element hooks. */
    const markdown_core_dialect *dialect;
    void *context;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* THE BUILDERS (docs/plans/2026-09-29-incremental-parsing.md, 5.2): the
     * blocks open in the active input, from its root to the deepest, the
     * current block. Each is the last child of the one before it and is not
     * yet one of that block's children: a block joins its parent's children
     * when it SETTLES, as its parent takes its next child or closes
     * (blocks.c). They are a stack linked through their scaffolds (node.h,
     * THE SCAFFOLD): a builder's `parent` is the builder it is open inside,
     * and its `last` the builder open inside it, none for the current block.
     * `block_root` is the active input's root -- the document, or a table
     * cell whose content is the input -- which the input's end closes in
     * place, and `current` the current block. `pending` is the block that
     * closed last, a child of the current block, until it settles: the lines
     * after its close still write it -- a blank line its LAST_LINE_BLANK, a
     * block identifier its anchor and end, a caption its table -- and nothing
     * writes a settled block but completion, which makes its inline
     * content. The document and the cell inputs share this parser and all
     * document registries. Input roots stay owned by the AST; the queue only
     * borrows them until their block content is read. */
    struct markdown_core_node *block_root, *current;
    struct markdown_core_node *pending;
    struct markdown_core_block_input *block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    /* The kinds of the blocks that hold the active input's root from outside
     * its input, as block kind bits (node.h, markdown_core_node_block_kind_bit):
     * none for the document, and a cell's `enclosing` while its content is
     * read. */
    uint32_t input_enclosing;
    /* The active input: the document's text, or a table cell's content
     * while its blocks are read. */
    markdown_core_parser_input input;
    /* The input that is not active: the storage the cells' inputs reuse
     * while the document's input is read, kept for the next cell. */
    markdown_core_parser_input idle_input;
    size_t input_line_work;
    /* A complete candidate may consume through a later source boundary. The
     * source driver advances to it after the current line has finished. */
    const unsigned char *claimed_cursor;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* See the documentation for markdown_core_parser_get_line_number() in markdown_core.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in markdown_core.h */
    bufsize_t offset;
    /* See the documentation for markdown_core_parser_get_column() in markdown_core.h */
    bufsize_t column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    bufsize_t first_nonspace;
    /* See the documentation for markdown_core_parser_get_first_nonspace_column() in markdown_core.h
     */
    bufsize_t first_nonspace_column;
    bufsize_t thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* See the documentation for markdown_core_parser_has_partially_consumed_tab() in
     * markdown_core.h */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* See the documentation for markdown_core_parser_get_last_line_end() in
     * markdown-core-element-api.h */
    bufsize_t last_line_end;
    /* Where the line being processed ends in the source, before its line
     * ending: the driver records it as it hands the line to the block
     * parser, and it holds while `curline` does. */
    bufsize_t line_end;
    /* Where input line `line_number` starts in the active input: the driver
     * records it with `line_end`, and a claim of later lines moves it. */
    bufsize_t line_start;
    /* Options set by the user, see the Options section in markdown_core.h */
    /* Sticky allocation-failure flag: once any parse structure is lost, the
     * one-shot transaction reports the whole parse as failed (NULL) instead of
     * returning a silently truncated document. */
    markdown_core_parse_error error;
    /* THE ENGINE'S WORK COUNTERS, for deterministic complexity gates. An
     * element counts its own work in its own state record. */
    size_t opaque_scan_work;
    size_t footnote_body_work;
    /* Run bytes, opener comparisons, and child moves in the shared delimiter algorithm. */
    size_t delimiter_work;
    /* Delimiter entries pushed, against which the pool's growth is measured. */
    size_t delimiter_pushes;
    /* Inline placements asked of the content-to-source map, and the runs
     * probed to answer the ones that left the cursor's run. The map's whole
     * claim since #346 is that a placement made while a container is read
     * left to right is measured in the cursor's run and probes nothing, and
     * one that leaves it probes a bounded number of runs rather than
     * searching the container's from scratch; the ratio is that claim. */
    size_t content_mark_queries;
    size_t content_mark_probes;
    /* Elements the inline-content hook dispatch EXAMINED, counted one per
     * element per family per inline-content node. The projection's whole claim
     * is that this grows with the declarers and not with the dialect, and
     * nothing else can see the difference: a dispatch that went back to
     * scanning every attached element would build the identical tree. So the
     * invariant is asserted on this counter rather than on output. */
    size_t inline_hook_work;
    /* What the parse continues, and the storage it borrows from its caller
     * (the revision's pool): every node it makes and every resource a
     * definition, a link or a heading's implicit reference states is a slot
     * of this pool's slabs, and every one it releases goes back here. */
    markdown_core_revision *revision;
    markdown_core_node_pool *pool;
    /* The lines the block-start lookahead visited plus the prefix bytes each
     * visit matched itself, for its linearity gate. */
    size_t block_lookahead_work;
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; markdown_core_parser_input_ends answers true
     * for it once the input has run out. */
    const unsigned char *lookahead_cursor;
    /* Whether a reading of the document's input reached its end, and where
     * the line that asked starts in the source: the line being processed, or
     * the first line for the document's prefix. A line whose decisions read
     * the end holds only while no more text follows
     * (markdown_core_parser_input_ends). */
    bool end_seen;
    bufsize_t end_seen_at;
    /* The open containers a lookahead matches, root first, with the flags
     * each had when it began: the builders from the active input's root to
     * the block's parent (markdown_core_parser_lookahead_begin). Owned by the
     * parser so a document with many candidates allocates them once. */
    struct markdown_core_lookahead_container *lookahead_chain;
    int lookahead_chain_alloc;
    /* Delimiter entries removed from an inline parse, kept for the next push
     * (see `markdown_core_inline_push_delimiter_entry`); linked through `next`
     * and released with the parser. */
    struct delimiter *free_delimiters;
    /* The workspace every attribute value of the parse is read into before
     * it is laid out (core/attributes.h); released with the parser. */
    markdown_core_attribute_scratch attribute_scratch;
    /* WHICH KINDS THIS PARSE PRODUCED, recorded where they are produced.
     *
     * Every node creation and every `set_kind` that a parse performs writes
     * here, so the gate on a finish step is read from a record rather than
     * gathered by a walk: a step reads it at each event it is asked at
     * (markdown_core_finish_step_admitted), against the kinds produced so
     * far. Gathering it by a walk instead is what forced the finish stage to
     * traverse the document twice. See the gate in `S_finish_parse` for what
     * the set over-approximates and why that is sound.
     *
     * Every production creation site goes through `markdown_core_parser_note_kind`;
     * `scripts/audit/check-parser-kind-record.mjs` holds that. */
    markdown_core_node_kind_set kinds_created;
    /* The content-to-source map (see markdown_core_line_mark). It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before the transaction returns -- and it is
     * released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
};

/* THE RUN AN OFFSET LIES IN, FOUND FROM WHERE THE LAST ONE WAS.
 *
 * A node's runs are contiguous in the parser's vector and ordered by content
 * offset, so the run containing an offset is the last whose start is at or
 * before it. `hint` is the run the caller last resolved, or the node's first
 * run for a caller that keeps none: the inline parser's cold path reads its
 * container left to right, so the answer is almost always the run the
 * previous answer named or the one after it -- a Text never crosses a line
 * ending (a break is its own node), and the next token starts where the last
 * one ended. Those two runs are probed first. Only an offset farther ahead --
 * an opaque span across many lines -- or behind the hint -- a Link placed
 * back at its opener, a rewind -- is searched for, over the part of the run
 * on that side, so no query costs more than the search alone did.
 *
 * ONE SEARCH FOR ONE QUESTION: the block phase asks it from the first run
 * and the placement's cold path from its cursor, through the same body. The
 * probes are handed back rather than counted here, so the accounting that
 * bounds the placement (the api test) is the placement's and a block-phase
 * caller inlines the search alone. */
static MARKDOWN_CORE_INLINE int markdown_core_block_content_mark_near(const markdown_core_parser *parser,
                                                                      const markdown_core_content_map *map,
                                                                      bufsize_t offset, int hint, size_t *probes) {
    const markdown_core_line_mark *marks = parser->line_marks;
    int lo = map->first, hi = lo + map->count - 1;
    int at = hint >= lo && hint <= hi ? hint : lo;
    size_t probed = 1;
    if (marks[at].content_offset <= offset) {
        if (at != hi && marks[at + 1].content_offset <= offset) {
            probed++;
            at++;
            if (at != hi && marks[at + 1].content_offset <= offset) {
                lo = at + 1;
                at = -1;
            }
        }
    } else {
        hi = at - 1;
        at = -1;
    }
    if (at < 0) {
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            probed++;
            if (marks[mid].content_offset <= offset) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        at = lo;
    }
    *probes += probed;
    return at;
}

/* The same question from a caller that keeps no cursor and counts nothing. */
static MARKDOWN_CORE_INLINE int markdown_core_block_content_mark_at(const markdown_core_parser *parser,
                                                                    const markdown_core_content_map *map,
                                                                    bufsize_t offset) {
    size_t probes = 0;
    return markdown_core_block_content_mark_near(parser, map, offset, map->first, &probes);
}

/* Resolve both ends of [from, to] against `node`'s map, each found from the
 * run before it: `cursor`, when given, is the run the caller last resolved
 * and is left on the run that answers `to`; a cursor outside the node's run,
 * a zeroed one included, is a hint that is simply not taken.
 * Returns 0 when the node has no map at all, in which case neither end is
 * resolved. The caller owns what it does with the answer: the run indices are
 * handed back rather than written onto a node, because whether a slice is
 * taken at all is a decision only the caller can make -- writing
 * `content_map.count` on a node that is not a verbatim copy of its source
 * would give every SPAN, LINK and EMPHASIS node a map it does not have, and
 * three places read that count as the question "is there a mapping". */
static MARKDOWN_CORE_INLINE int markdown_core_parser_content_span(markdown_core_parser *parser,
                                                                  const markdown_core_content_map *map, bufsize_t from,
                                                                  bufsize_t to, markdown_core_content_span *span,
                                                                  int *cursor) {
    span->has_start = false;
    span->has_end = false;
    span->first = span->last = 0;
    if (!parser || !map || map->count <= 0) {
        return 0;
    }
    int hint = cursor ? *cursor : map->first;
    size_t probes = 0;
    if (from >= 0) {
        bufsize_t offset = from + map->offset;
        span->first = markdown_core_block_content_mark_near(parser, map, offset, hint, &probes);
        const markdown_core_line_mark *mark = &parser->line_marks[span->first];
        span->start = mark->source + (offset - mark->content_offset) * mark->source_step;
        span->has_start = true;
        hint = span->first;
    }
    if (to >= 0) {
        bufsize_t offset = to + map->offset;
        span->last = markdown_core_block_content_mark_near(parser, map, offset, hint, &probes);
        hint = span->last;
        const markdown_core_line_mark *mark = &parser->line_marks[span->last];
        span->end = mark->source + (offset - mark->content_offset) * mark->source_step + mark->source_width;
        span->has_end = true;
    }
    if (cursor) {
        *cursor = hint;
    }
    parser->content_mark_probes += probes;
    return 1;
}

/* THE PARSE'S NODE OPERATIONS, WHICH RECORD THE KIND THEY PRODUCE.
 *
 * `kinds_created` decides which finish steps run at the events they were
 * projected to, so a production site that writes a kind without recording it
 * does not fail a build or a test: it makes the gate skip a step some
 * document needed, and the defect surfaces
 * as a missing rewrite far from the line that caused it. That is a bad thing
 * to police with an audit over twenty-one call sites, so it is not policed:
 * recording is part of producing a node, in the two operations below.
 *
 * `markdown_core_node_new` and `markdown_core_node_set_kind` stay for callers
 * that have no parse -- the tests build trees by hand -- and production code
 * uses these instead, which `scripts/audit/check-parser-kind-record.mjs` holds.
 *
 * The record OVER-APPROXIMATES, deliberately. A node the parse creates and
 * then discards leaves its bit set although the finished tree holds no such
 * node. Making it exact would mean observing REMOVAL, and the only way to do
 * that is another walk of the whole tree -- which is the cost this record
 * exists to avoid. Over-approximating can only make the gate skip fewer
 * steps, never miss one, because a kind in the finished tree was necessarily
 * created; and a step asked where none of its declared kinds is finds nothing
 * to do. */
static inline void markdown_core_parser_fail(markdown_core_parser *parser, markdown_core_parse_error error) {
    if (!parser->error) {
        parser->error = error;
    }
}

static inline void markdown_core_parser_note_kind(markdown_core_parser *parser, markdown_core_node_type kind) {
    if (parser) {
        markdown_core_node_kind_set_add(&parser->kinds_created, kind);
    }
}

/* A release gives the nodes' storage back to the parse's pool, where the
 * parse's next nodes take it; a caller with no parse frees as the public
 * function does. */
static inline void markdown_core_parser_release_node(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_node_pool_release(parser ? parser->pool : NULL, node);
}

/* Whether the walk asks `entry`'s step at the event it is projected to: its
 * gate, read against the kinds the parse has produced so far. */
static inline bool markdown_core_finish_step_admitted(const markdown_core_finish_step_entry *entry,
                                                      const markdown_core_parser *parser) {
    return !entry->gated || (entry->acts_on.blocks & parser->kinds_created.blocks) ||
           (entry->acts_on.inlines & parser->kinds_created.inlines);
}

static inline markdown_core_node *markdown_core_parser_make_node(markdown_core_parser *parser,
                                                                 markdown_core_node_type type) {
    markdown_core_parser_note_kind(parser, type);
    return markdown_core_node_pool_new(parser ? parser->pool : NULL, type, NULL);
}

static inline markdown_core_node *markdown_core_parser_make_node_with_ext(markdown_core_parser *parser,
                                                                          markdown_core_node_type type,
                                                                          const markdown_core_element *element) {
    markdown_core_parser_note_kind(parser, type);
    return markdown_core_node_pool_new(parser ? parser->pool : NULL, type, element);
}

static inline markdown_core_node_set_kind_result markdown_core_parser_set_node_kind(markdown_core_parser *parser,
                                                                                    markdown_core_node *node,
                                                                                    markdown_core_node *parent,
                                                                                    markdown_core_node_type kind) {
    markdown_core_parser_note_kind(parser, kind);
    return markdown_core_node_set_kind(node, parent, kind);
}

/* THE BUILDER STACK'S QUESTIONS (the builders, above), each answered from
 * one builder's scaffold. The current block. */
static inline markdown_core_node *markdown_core_parser_current(const markdown_core_parser *parser) {
    return parser->current;
}

/* Whether a block that closed has settled: every one has but the pending
 * block, which a later line can still write. */
static inline bool markdown_core_parser_settled(const markdown_core_parser *parser, const markdown_core_node *node) {
    return node != parser->pending;
}

/* The active input's root: the document, or the table cell whose content the
 * input is. */
static inline markdown_core_node *markdown_core_parser_block_root(const markdown_core_parser *parser) {
    return parser->block_root;
}

/* The builder `node`, an open block, is open inside: NULL for the active
 * input's root. */
static inline markdown_core_node *markdown_core_parser_builder_below(const markdown_core_parser *parser,
                                                                     const markdown_core_node *node) {
    return node == parser->block_root ? NULL : markdown_core_scaffold_parent(node);
}

/* The kinds of the blocks open on the builder stack, the current one down, and
 * of those that hold the active input's root, as block kind bits (node.h,
 * markdown_core_node_block_kind_bit). */
static inline uint32_t markdown_core_parser_open_kinds(const markdown_core_parser *parser) {
    uint32_t kinds = parser->input_enclosing;
    for (const markdown_core_node *open = parser->current; open;
         open = markdown_core_parser_builder_below(parser, open)) {
        kinds |= markdown_core_node_block_kind_bit((markdown_core_node_type)open->kind);
    }
    return kinds;
}

/* Whether `node`, an open block, or a block that holds it is of a kind in
 * `kinds`, a set of block kind bits (node.h, markdown_core_node_block_kind_bit):
 * the blocks open around it, and those that hold the active input's root. */
static inline bool markdown_core_parser_inside(const markdown_core_parser *parser, const markdown_core_node *node,
                                               uint32_t kinds) {
    if (parser->input_enclosing & kinds) {
        return true;
    }
    for (; node; node = markdown_core_parser_builder_below(parser, node)) {
        if (markdown_core_node_block_kind_bit((markdown_core_node_type)node->kind) & kinds) {
            return true;
        }
    }
    return false;
}

/* The block that holds `node`, an open block or the pending one; NULL for
 * the active input's root. */
static inline markdown_core_node *markdown_core_parser_parent(const markdown_core_parser *parser,
                                                              const markdown_core_node *node) {
    if (node == parser->pending) {
        return parser->current;
    }
    return markdown_core_parser_builder_below(parser, node);
}

/* The last child of `node`, an open block: the block open inside it, or,
 * when it is the current block, its pending child or else its last settled
 * one; NULL when it has none. */
static inline markdown_core_node *markdown_core_parser_last_child(const markdown_core_parser *parser,
                                                                  const markdown_core_node *node) {
    if (node != parser->current) {
        return markdown_core_scaffold_last(node);
    }
    if (parser->pending) {
        return parser->pending;
    }
    return node->child_count ? node->children[node->child_count - 1] : NULL;
}

/* The instance of the structure element of `node`'s kind (dialect.h,
 * markdown_core_dialect_structure): the `self` of every structure hook the
 * engine asks about the node. NULL for no node. */
static inline const markdown_core_element_instance *markdown_core_parser_structure(const markdown_core_parser *parser,
                                                                                   const markdown_core_node *node) {
    return markdown_core_dialect_structure(parser->dialect, node);
}

/* The record of `node`'s kind (dialect.h, markdown_core_kind_record): its
 * structure's instance and every fact of it the engine asks. */
static inline const markdown_core_kind_record *markdown_core_parser_kind(const markdown_core_parser *parser,
                                                                         const markdown_core_node *node) {
    return markdown_core_dialect_kind(parser->dialect, (markdown_core_node_type)node->kind);
}

/* Physical geometry is present for every visited line. Optional normalized
 * views and grammar facts share one lazily created record for that line. */
typedef struct markdown_core_input_line {
    /* Input buffers, hence offsets and line counts, are bounded by INT32_MAX / 2. */
    uint32_t start, end;
    /* One-based index; zero means no query needs optional state for this line. */
    uint32_t facts;
    /* Where the line's own bytes begin: the offset in the line where the
     * bytes of the last open block the line reached begin, past the
     * prefixes of the containers above it, as the block parser or a
     * lookahead last matched them (markdown_core_parser_line_places). */
    uint32_t own;
} markdown_core_input_line;

typedef struct markdown_core_line_facts {
    struct markdown_core_normalized_line *normalized;
    /* Table grammar search facts under one matched container prefix. */
    const struct markdown_core_node *table_container;
    int table_offset;
    unsigned table_absent;
    /* A candidate matches open-container prefixes on every line it visits.
     * Failed candidates reaching the same line have nested container chains,
     * so each resumes from the deepest match already recorded. Each
     * (container, line) prefix is matched at most once per parse. NULL means
     * that no scan has recorded a prefix. */
    const struct markdown_core_node *container;
    /* Its distance from the document root: chain[depth] == container. */
    int depth;
    /* The line state after that container's prefix: what S_advance_offset left. */
    bufsize_t offset;
    bufsize_t column;
    bool partially_consumed_tab;
    /* A list's second consecutive blank line at indentation zero: the innermost
     * open block owns the whole line and nothing below the list is asked. */
    bool taken;
    /* Blank after the recorded container's prefix. */
    bool blank;
    /* On the first line of a run of blank lines: the number of the first line
     * after the run and where it begins (`run_end_at`), so a later scan
     * whose extra containers accept every blank line steps over the run at
     * once. 0 when not a run. */
    int run_end;
    /* Only NUL-bearing lines need this count; it occupies former padding. */
    uint32_t nul_count;
    /* Where the first line after the run begins, as an offset in the input:
     * a session's text moves when it grows, and the facts stay. */
    size_t run_end_at;
} markdown_core_line_facts;
/* The index is a contiguous prefix. Its next record already owns this line's
 * continuation; at the frontier the scanner owns it. No newline bytes need
 * rereading and no third offset needs retaining on every physical line. */
static inline size_t markdown_core_input_line_next(const markdown_core_parser *parser,
                                                   const markdown_core_input_line *line) {
    const markdown_core_input_line *next = line + 1;
    return next < parser->input.lines + parser->input.line_count ? next->start : parser->input.scanned;
}

/* Returned pointers are borrowed until the next request that grows the
 * index. Keep line numbers or copies across such a request. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index);

static inline markdown_core_input_line *markdown_core_parser_source_line(markdown_core_parser *parser, int line) {
    if (line < parser->input.first_line || parser->error) {
        return NULL;
    }
    size_t index = (size_t)(line - parser->input.first_line);
    return index < parser->input.line_count ? &parser->input.lines[index]
                                            : markdown_core_parser_extend_source_lines(parser, index);
}

/* THE END OF THE ACTIVE INPUT, learned in one place. Every reading that
 * asks whether the input goes on past a position asks here, or asks for a
 * line after the ones the driver reached (markdown_core_parser_source_line);
 * nothing else compares a position with the input's end. `cursor` is a
 * position of the active input. A reading at or past the end of the last
 * complete line reads what more text can change, so when the input is the
 * document's the parser records the line that asked (`end_seen`): its
 * decisions hold only while no more text follows. A table cell's input is
 * complete when it is read, so its end records nothing. */
static inline void markdown_core_parser_see_end(markdown_core_parser *parser) {
    if (!parser->end_seen && markdown_core_parser_block_root(parser) == parser->root) {
        parser->end_seen = true;
        parser->end_seen_at = parser->line_start;
    }
}

static inline bool markdown_core_parser_input_ends(markdown_core_parser *parser, const unsigned char *cursor) {
    const unsigned char *end = parser->input.source + parser->input.size;
    assert(cursor >= parser->input.source && cursor <= end);
    if (cursor >= parser->input.source + parser->input.complete) {
        markdown_core_parser_see_end(parser);
    }
    return cursor == end;
}

/* Optional state is sparse within the input index: properties and ordinary
 * driver visits need only geometry unless they normalize a NUL-bearing line.
 * Both vectors reset with their input and retain capacity until disposal. */
markdown_core_line_facts *markdown_core_parser_extend_line_facts(markdown_core_parser *parser,
                                                                 markdown_core_input_line *line);
static inline markdown_core_line_facts *markdown_core_parser_get_line_facts(markdown_core_parser *parser, int number) {
    markdown_core_input_line *line = markdown_core_parser_source_line(parser, number);
    if (!line) {
        return NULL;
    }
    return line->facts ? &parser->input.facts[line->facts - 1] : markdown_core_parser_extend_line_facts(parser, line);
}

/* A container of a lookahead's chain, and its flags as the lookahead began. */
typedef struct markdown_core_lookahead_container {
    struct markdown_core_node *node;
    markdown_core_node_internal_flags flags;
} markdown_core_lookahead_container;

/* A NON-CONSUMING LOOKAHEAD over the lines after the one being processed.
 *
 * A block start whose grammar reaches past its own line -- the `%%` block
 * comment, which is a paragraph line unless a closer line follows under the
 * same container prefixes -- decides here before it opens anything, so a
 * candidate that fails consumes nothing and the block parser never rewinds.
 * Each line is offered exactly as the block parser will see it once the block
 * exists: the open containers' prefixes matched by the same matchers
 * `check_open_blocks` runs, through the same parser cursor, with a list item
 * whose first child is the block about to be added accepting a blank line the
 * way it will then. A line that does not carry every prefix, a container's
 * own closing line, or the end of the input ends the lookahead. Blank lines
 * are stepped over and counted, because no block start begins on one.
 *
 * `begin` saves the parser's line state and the chain's flags, `end` restores
 * them; nothing else in the parser or the tree is touched. `begin` returns
 * false only when an allocation failed, and has then marked the parse lost. */
typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_node *parent;
    int depth;
    const unsigned char *cursor;
    int line;
    int run_start;
    bufsize_t saved_offset;
    bufsize_t saved_column;
    bufsize_t saved_first_nonspace;
    bufsize_t saved_first_nonspace_column;
    int saved_indent;
    bool saved_blank;
    bool saved_partially_consumed_tab;
    bool active;
} markdown_core_block_lookahead;

/* Stable source-coordinate ordering, shared by deferred nodes and cell geometry. */
void markdown_core_source_order_dispose(markdown_core_source_order *workspace);
/* The walk stack with room for `count` entries of `size` bytes, keeping what
 * it holds, or NULL when that much cannot be allocated. */
void *markdown_core_parser_walk_stack(markdown_core_parser *parser, size_t count, size_t size);
int markdown_core_order_source_entries(markdown_core_source_order *workspace, void *entries, size_t count,
                                       size_t stride, uint64_t (*key)(const void *));

struct markdown_core_block_reader;
/* Query the ordinary block-start rules before the table slot. Paragraph
 * continuation uses its real interruption rules (notably list starts, type-7
 * HTML and indentation); following lines come from the caller's source view. */
bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_node *parent,
                                          markdown_core_chunk *input, int first, int column, int indent, bool paragraph,
                                          struct markdown_core_block_reader *reader);

/* Close every block open above `container` on the builder stack, the
 * current one first, so the container is the current block (THE BUILDERS,
 * above). */
void markdown_core_parser_close_below(markdown_core_parser *parser, struct markdown_core_node *container);
/* Schedule an already owned node's mapped content for the ordinary block
 * parser, to be read inside blocks of the kinds `enclosing`: the open kinds
 * (markdown_core_parser_open_kinds) as the node was made. No nested parse
 * transaction, document, dialect or C recursion. */
bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner,
                                            uint32_t enclosing);
/* WHERE A BYTE OF THE ACTIVE INPUT WAS WRITTEN, as a byte offset of the
 * document source. `line` is a line of the active input and `column` a byte
 * column of that line as the block parser reads it, counted from 1: the
 * document's own line, or a line of a table cell's content, which the parser
 * reads as an input of its own and whose bytes the cell's map places back in
 * the document. `source_offset` answers the byte at `column`, and
 * `source_end` the offset just after it, so a range whose last byte is at
 * `column` ends there; column zero is the start of the line.
 * Producers call these when placing a node. */
/* The answers for a line that is not a copy of its source: a document line
 * that carries NUL, or a line of a cell's content. */
bufsize_t markdown_core_parser_mapped_source_offset(markdown_core_parser *parser, int line, int column);
bufsize_t markdown_core_parser_mapped_source_end(markdown_core_parser *parser, int line, int column);

/* The geometry of input line `line`, which the driver has already visited. */
static inline markdown_core_input_line *markdown_core_parser_visited_line(const markdown_core_parser *parser,
                                                                          int line) {
    size_t index = (size_t)(line - parser->input.first_line);
    assert(line >= parser->input.first_line && index < parser->input.line_count);
    return &parser->input.lines[index];
}

/* An input that is not mapped is a copy of its source, so a line's column c
 * is the byte c - 1 after the line's start. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bufsize_t
    markdown_core_parser_source_offset(markdown_core_parser *parser, int line, int column) {
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    return !parser->input.mapped ? (bufsize_t)geometry->start + column - 1
                                 : markdown_core_parser_mapped_source_offset(parser, line, column);
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bufsize_t
    markdown_core_parser_source_end(markdown_core_parser *parser, int line, int column) {
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    return !parser->input.mapped ? (bufsize_t)geometry->start + column
                                 : markdown_core_parser_mapped_source_end(parser, line, column);
}
/* Where the first byte of input line `line` was written: the document line's
 * own start, or, in a table cell, the source of the cell's first byte on that
 * line (its line ending, when the cell is blank there). Lines of one input
 * begin in increasing source order, so a node began on `line` or later when
 * its start is at least this offset. */
bufsize_t markdown_core_parser_line_offset(markdown_core_parser *parser, int line);
/* THE PLACES OF THE OWN LINES OF `node`, a block of `container`, which
 * starts on input line `*line`, an earlier one or a later one of the lines
 * visited: on each line its place spans, a place of length 0 over the source
 * from where the line's own bytes begin to where the line ends, through its
 * terminator on a line of the document itself, those that touch joined. A
 * node whose lines join into one place keeps none, and a block of the
 * document itself, which lies on whole lines of the source, has none. The
 * places hold absolute source ranges, which reading the node's content fills
 * with its content places (markdown_core_parser_read_content) and publishing
 * turns into its runs (node.h). `*line` becomes the line the node ends on,
 * where a later node can start the search. */
void markdown_core_parser_line_places(markdown_core_parser *parser, markdown_core_node *node,
                                      const markdown_core_node *container, int *line);
/* Whether `node` begins on input line `line`, a line of the active input the
 * driver has reached. */
bool markdown_core_parser_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
/* An inline root's content, `length` bytes of it read, becomes the input its
 * nodes are placed in: its map to the source is kept in its places, among
 * the places of length 0 of its own source, and its map becomes the identity
 * (blocks.c). */
void markdown_core_parser_read_content(markdown_core_parser *parser, markdown_core_node *node, bufsize_t length);
int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset);
/* Where input line `line` starts in the active input. */
static inline bufsize_t markdown_core_parser_line_start(const markdown_core_parser *parser, int line) {
    return (bufsize_t)markdown_core_parser_visited_line(parser, line)->start;
}
/* The same runs for a caller that holds where input line `line` starts in the
 * active input. */
int markdown_core_parser_append_line_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                           bufsize_t line_start, int column, bufsize_t length, bufsize_t offset);

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, struct markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead);
/* The next non-blank line that carries the prefixes: its bytes through its
 * line ending, the index of its first non-space byte, its indentation after
 * the prefixes, and the blank lines stepped over before it. Returns 0 when the
 * lookahead has ended. */
int markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, markdown_core_chunk *line,
                                        int *first_nonspace, int *indent, int *blank_lines);
void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead);

/* THE DEFINITION LOOKUP of inline content: the record of `kind` that
 * declares `label`, as written, for the block being completed, or NULL.
 * Reference and footnote labels are looked up in their normal form, and a
 * specimen label as its bytes (registry.h). At a finalize every declaration
 * answers; at a snapshot only one whose key some definition declares before
 * the block starts, which is before its inline roots start, and a lookup that
 * finds none is recorded for the finalize. `*failed` is set when storage
 * could not be had, and the answer is then NULL. */
const markdown_core_registry_record *markdown_core_parser_find_definition(markdown_core_parser *parser,
                                                                          markdown_core_registry_kind kind,
                                                                          const markdown_core_chunk *label,
                                                                          bool *failed);

/* THE LONGEST SOURCE A PARSE TAKES: offsets are int32, and every buffer
 * derived from the source stays under half of that. A session refuses a text
 * that would grow past it (session.c); below it, `length` is within this
 * bound. */
#define MARKDOWN_CORE_SOURCE_CAPACITY ((size_t)(INT32_MAX / 2))

/* The engine has one parse operation, run by a parser instance. An instance
 * parses with the dialect `elements` names, in that order; the composition
 * root that chooses the product's dialect lives with the elements
 * (markdown-core-elements.h), so the engine names no element. `setup`, when
 * present, extends the dialect the instance will parse with: it receives the
 * builder, already holding `elements`, and never the parser, so what it
 * registers is sealed before any source is read and fixed for the instance's
 * lifetime (dialect.h). `context` is handed to setup and carried on the
 * parser for element hooks. NULL when setup returns false or the instance
 * could not be allocated. */
markdown_core_parser *markdown_core_parser_create(const markdown_core_element *const *elements, size_t count,
                                                  markdown_core_parser_setup_func setup, void *context);
/* A PARSE IS A START, COMMITS AND A FINALIZE, one transaction at a time on
 * an instance. `start` begins a transaction that continues what `revision`
 * says, with the storage it lends (above). `commit` reads the lines of
 * `source` that start before `limit`, from where the transaction's last
 * commit stopped: the text of every commit of a transaction is the one
 * before it with bytes added at its end, and it is borrowed until the next
 * commit or the end of the transaction; false when the transaction failed.
 * `finalize` ends the input there:
 * every block still open closes, the closed blocks complete, and the tree is
 * published and returned, or NULL when the transaction failed. `discard`
 * ends a transaction and returns nothing. Both release the transaction's
 * state, so an instance runs any number of them, each as the first. */
void markdown_core_parser_start(markdown_core_parser *parser, markdown_core_revision *revision);
bool markdown_core_parser_commit(markdown_core_parser *parser, const char *source, size_t length, size_t limit);
markdown_core_node *markdown_core_parser_finalize(markdown_core_parser *parser);
void markdown_core_parser_discard(markdown_core_parser *parser);
/* THE COMMIT POINT the lines read so far leave (plan 5.1, 5.3): where the
 * first line that read the end of the input starts, or the end of the last
 * complete line when none did. Every line before it decided only from bytes
 * that more text leaves as they are, so a commit with this limit reads them
 * as a parse of any longer text does. */
size_t markdown_core_parser_commit_point(const markdown_core_parser *parser);
/* Completes the blocks that have settled, for a snapshot (plan 5.4): the
 * cells of the tables that closed are read, the document prepares the
 * headings that settled, and the completion queue is completed with a
 * snapshot's lookups. What it completes stays complete. False when the
 * transaction failed. */
bool markdown_core_parser_complete(markdown_core_parser *parser);
/* A finalize whose tree is a snapshot (`snapshot` above). */
markdown_core_node *markdown_core_parser_snapshot(markdown_core_parser *parser);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
