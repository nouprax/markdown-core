#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <assert.h>
#include <stdint.h>
#include "references.h"
#include "node.h"
#include "buffer.h"
#include "dialect.h"
#include "text_tree.h"

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
 * `previous` is the root of a tree a parse published, and `edits` turn the
 * text it was parsed from into the text this parse reads, in its
 * coordinates: disjoint, in source order. The published tree continues it
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.9): every node matched to
 * an old node takes its id, every other node takes the next id after
 * `last_id`, and a matched node equal to its old node as a value is that old
 * node. On success the parse owns `previous`: it is the returned root, or it
 * is released into `pool` with every other node it retires, and `last_id` is
 * the last id issued. `node_count` is the number of nodes in `previous`,
 * and on success the number in the published tree. A fresh parse continues
 * nothing: `previous` is NULL and `last_id` is 0, so its root is 1 and every
 * node is numbered in canonical walk order.
 *
 * `pool` lends the parse every node and resource slot it takes (node.h); it
 * outlives the parse, and its owner disposes it. */
typedef struct markdown_core_revision {
    markdown_core_node_pool *pool;
    markdown_core_node *previous;
    const markdown_core_byte_edit *edits;
    size_t edit_count;
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
     * synthetic inline runs use zero because they never finalize as blocks. */
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

/* The specimen definitions of one parse, in the order they were committed,
 * for the index citations resolve against. Every definition is owned by the
 * block tree; the collection only borrows it until the document finishes. */
typedef struct {
    struct markdown_core_node **values;
    size_t count;
    size_t capacity;
} markdown_core_definition_collection;

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

struct markdown_core_parser {
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    markdown_core_source_order source_order;
    /* The stack the finish stage's tree walks borrow in turn, the finish
     * walk's frames and then publishing's: each grows it to what it needs
     * (markdown_core_parser_walk_stack) and leaves it to the next, and the
     * stage releases it when it ends. */
    void *walk_stack;
    size_t walk_stack_size;
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
    /* The active block grammar boundary. The document and mapped cell inputs
     * share this parser and all document registries. Input roots stay owned by
     * the AST; the queue only borrows them until their block content is read. */
    struct markdown_core_node *block_root;
    struct markdown_core_node *matched_container;
    struct markdown_core_node **block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    /* Geometry and grammar facts for the active immutable input. The driver
     * and lookahead extend one index; a source byte is scanned for line
     * geometry once, whether the input is the document or a mapped cell. */
    const unsigned char *input_source;
    size_t input_length, input_scanned;
    struct markdown_core_input_line *input_lines;
    struct markdown_core_normalized_line *normalized_lines;
    struct markdown_core_line_facts *input_facts;
    size_t input_fact_count, input_fact_capacity;
    size_t input_line_count, input_line_capacity;
    /* Whether a column of the active input may stand elsewhere than the
     * same offset from its line's start in the source: true for a cell's
     * content, and for the document once its scan finds NUL, which the block
     * parser reads as the three bytes of U+FFFD. */
    bool input_mapped;
    int input_first_line;
    size_t input_line_work;
    /* A complete candidate may consume through a later source boundary. The
     * source driver advances to it after the current line has finished. */
    const unsigned char *claimed_cursor;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
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
    size_t definition_registration_work;
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
    /* THE FINISH STAGE'S TRAVERSAL COUNT, in numbers the output cannot show.
     * The stage's whole claim is that it walks each owned root ONCE and runs
     * inline completion, consolidation and every finish step from inside that
     * one walk; a stage that walked a root once per hook would build the
     * identical tree, so the claim is asserted on these rather than on a dump.
     *
     * `finish_walk_events` is every iterator step the finish stage took: the
     * walk's own ENTER, EXIT and DONE events, plus the ENTER and EXIT that
     * consolidation advances over when it absorbs a following Text sibling
     * (those nodes are visited -- by consolidation, which completes and frees
     * them -- and counted as visited). Repositioning the cursor back to the
     * survivor's EXIT is not a step: that event was already delivered.
     * `finish_nodes_entered` is the ENTER events among them, absorbed siblings
     * included; `finish_walk_roots` is the DONE events, one per root walked.
     *
     * The denominator is taken without a traversal, at the two seams where
     * nodes come and go. `nodes_created` counts every node a parse makes, at
     * the same operation that records the node's kind, so the audit that
     * holds one holds the other; `nodes_freed` counts every node a parse
     * releases through `markdown_core_parser_release_node`, which the finish
     * stage's every free takes -- consolidation's, and each step's, which the
     * finish-hook audit holds -- and which counts the descendants and field
     * roots that go with a node, since the release loop visits each of them.
     * The walk notes both in `..._before_finish` as it starts. The finished
     * tree holds every node that existed when the walk started, plus those
     * the walk's inline parsing handed it (`finish_nodes_parsed`, which the
     * walk enters), less those the stage freed, plus those its steps made
     * (which it never enters), so one traversal per root is exactly
     *
     *   finish_nodes_entered == nodes in the finished tree
     *                           + (nodes_freed - nodes_freed_before_finish)
     *                           - (nodes_created - nodes_created_before_finish)
     *                           + finish_nodes_parsed
     *
     * where the finished tree is counted by whoever holds it (the api test
     * walks it with the public iterator and the owned-subtree visitors), and
     * a stage that walked each root k times, counting as the engine's walks
     * count, would enter k times as many. `finish_walk_events == 2 *
     * finish_nodes_entered + finish_walk_roots` then says that every step
     * taken was one of those events. A whole-root consolidation driven through
     * the public entry point with a parser adds exactly one traversal of that
     * root to the events, the entered and the roots.
     *
     * The count sees only the walks that report themselves: the engine's
     * finish walk and that public entry point. A traversal that keeps no count
     * -- an iterator a step opened over its node's subtree -- is invisible
     * here, so the other half of the invariant is held on the source:
     * scripts/audit/check-finish-hook-shapes.mjs refuses a translation unit that
     * declares a finish step and opens an iterator. */
    size_t nodes_created, nodes_created_before_finish;
    size_t nodes_freed, nodes_freed_before_finish;
    /* What the parse continues, and the storage it borrows from its caller
     * (the revision's pool): every node it makes and every resource a
     * definition, a link or a heading's implicit reference states is a slot
     * of this pool's slabs, and every one it releases goes back here. */
    markdown_core_revision *revision;
    markdown_core_node_pool *pool;
    /* The nodes the walk's own inline parsing handed it, at the ENTER of each
     * container it parsed: what the parse made less what it discarded before
     * returning (a bracket's opener text, a token that failed to close), which
     * is why every parse-time release is counted (the kind-record audit holds
     * that). Made after the walk started, so the identity above adds them
     * back. */
    size_t finish_nodes_parsed;
    size_t finish_walk_events;
    size_t finish_nodes_entered;
    size_t finish_walk_roots;
    /* The lines the block-start lookahead visited plus the prefix bytes each
     * visit matched itself, for its linearity gate. */
    size_t block_lookahead_work;
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; `cursor == end` once the input has run out. */
    const unsigned char *lookahead_cursor;
    const unsigned char *lookahead_end;
    /* The input's last line as the block parser will see it, normalized once
     * and reused by every lookahead that reaches it: it has no terminator of
     * its own in the source, and a line handed to the prefix matchers must
     * end in one. */
    markdown_core_strbuf lookahead_last_line;
    bool lookahead_last_line_ready;
    /* The open containers a lookahead matches, root first, with the flags they
     * had when it began; and the per-line resume cache. Both are owned by the
     * parser so a document with many candidates allocates them once. */
    struct markdown_core_node **lookahead_chain;
    markdown_core_node_internal_flags *lookahead_chain_flags;
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
     * here, so the gate on a finish hook is read from a record rather than
     * gathered by a walk: a pass is selected once the finish walk -- which
     * parses the inline content -- has completed, and a step reads it at
     * each event it is asked at (markdown_core_finish_step_entry). Gathering
     * it by a walk instead is what forced the finish stage to traverse the
     * document twice. See the gate in `S_finish_parse` for what the set
     * over-approximates and why that is sound.
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
 * `kinds_created` decides which finish hooks run -- a global pass, a step at
 * every event it was projected to -- so a production site that writes a kind
 * without recording it does not fail a build or a test: it makes the gate skip
 * a hook some document needed, and the defect surfaces
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
 * passes, never miss one, because a kind in the finished tree was necessarily
 * created; and a pass that runs over a tree holding none of its declared kinds
 * finds nothing to do. */
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

/* A creation records the kind and counts the node: the count is the finish
 * stage's denominator (the traversal counters above). */
static inline void markdown_core_parser_note_node(markdown_core_parser *parser, markdown_core_node_type kind) {
    if (parser) {
        markdown_core_node_kind_set_add(&parser->kinds_created, kind);
        parser->nodes_created++;
    }
}

/* A release counts what it freed, for the same denominator; a caller with no
 * parse frees as the public function does. */
static inline void markdown_core_parser_release_node(markdown_core_parser *parser, markdown_core_node *node) {
    size_t released = markdown_core_node_pool_release(parser ? parser->pool : NULL, node);
    if (parser) {
        parser->nodes_freed += released;
    }
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
    markdown_core_parser_note_node(parser, type);
    return markdown_core_node_pool_new(parser ? parser->pool : NULL, type, NULL);
}

static inline markdown_core_node *markdown_core_parser_make_node_with_ext(markdown_core_parser *parser,
                                                                          markdown_core_node_type type,
                                                                          const markdown_core_element *element) {
    markdown_core_parser_note_node(parser, type);
    return markdown_core_node_pool_new(parser ? parser->pool : NULL, type, element);
}

static inline markdown_core_node_set_kind_result markdown_core_parser_set_node_kind(markdown_core_parser *parser,
                                                                                    markdown_core_node *node,
                                                                                    markdown_core_node_type kind) {
    markdown_core_parser_note_kind(parser, kind);
    return markdown_core_node_set_kind(node, kind);
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
     * lookahead last matched them (markdown_core_parser_place_pieces). */
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
     * after the run and where it begins, so a later scan whose extra containers
     * accept every blank line steps over the run at once. 0 when not a run. */
    int run_end;
    /* Only NUL-bearing lines need this count; it occupies former padding. */
    uint32_t nul_count;
    const unsigned char *run_end_cursor;
} markdown_core_line_facts;
/* The index is a contiguous prefix. Its next record already owns this line's
 * continuation; at the frontier the scanner owns it. No newline bytes need
 * rereading and no third offset needs retaining on every physical line. */
static inline size_t markdown_core_input_line_next(const markdown_core_parser *parser,
                                                   const markdown_core_input_line *line) {
    const markdown_core_input_line *next = line + 1;
    return next < parser->input_lines + parser->input_line_count ? next->start : parser->input_scanned;
}

/* Returned pointers are borrowed until the next request that grows the
 * index. Keep line numbers or copies across such a request. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index);

static inline markdown_core_input_line *markdown_core_parser_source_line(markdown_core_parser *parser, int line) {
    if (line < parser->input_first_line || parser->error) {
        return NULL;
    }
    size_t index = (size_t)(line - parser->input_first_line);
    return index < parser->input_line_count ? &parser->input_lines[index]
                                            : markdown_core_parser_extend_source_lines(parser, index);
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
    return line->facts ? &parser->input_facts[line->facts - 1] : markdown_core_parser_extend_line_facts(parser, line);
}

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

/* Schedule an already owned node's mapped content for the ordinary block
 * parser. No nested parse transaction, document, dialect or C recursion. */
void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser);
bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner);
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
    size_t index = (size_t)(line - parser->input_first_line);
    assert(line >= parser->input_first_line && index < parser->input_line_count);
    return &parser->input_lines[index];
}

/* An input that is not mapped is a copy of its source, so a line's column c
 * is the byte c - 1 after the line's start. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bufsize_t
    markdown_core_parser_source_offset(markdown_core_parser *parser, int line, int column) {
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    return !parser->input_mapped ? (bufsize_t)geometry->start + column - 1
                                 : markdown_core_parser_mapped_source_offset(parser, line, column);
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bufsize_t
    markdown_core_parser_source_end(markdown_core_parser *parser, int line, int column) {
    const markdown_core_input_line *geometry = markdown_core_parser_visited_line(parser, line);
    return !parser->input_mapped ? (bufsize_t)geometry->start + column
                                 : markdown_core_parser_mapped_source_end(parser, line, column);
}
/* Where the first byte of input line `line` was written: the document line's
 * own start, or, in a table cell, the source of the cell's first byte on that
 * line (its line ending, when the cell is blank there). Lines of one input
 * begin in increasing source order, so a node began on `line` or later when
 * its start is at least this offset. */
bufsize_t markdown_core_parser_line_offset(markdown_core_parser *parser, int line);
/* THE PIECES OF `node`, a block of `container`, which starts on input line
 * `*line`, an earlier one or a later one of the lines visited: on each line
 * its place spans, the source from where the line's own bytes begin to where
 * the line ends, through its terminator on a line of the document itself,
 * those that touch joined. A node with fewer than two pieces keeps none, and
 * a block of the document itself, which lies on whole lines of the source,
 * has one. Its pieces hold absolute source ranges, which publishing clips to
 * its place (node.h). `*line` becomes the line the node ends on, where a
 * later node can start the search. */
void markdown_core_parser_place_pieces(markdown_core_parser *parser, markdown_core_node *node,
                                       const markdown_core_node *container, int *line);
/* Whether `node` begins on input line `line`, a line of the active input the
 * driver has reached. */
bool markdown_core_parser_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
/* An inline root's content, `length` bytes of it read, becomes the input its
 * nodes are placed in: its map to the source is kept as its runs, and its map
 * becomes the identity (blocks.c). */
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

/* Register an element's committed definition in its borrowed parse index.
 * The block tree keeps ownership. Allocation failure aborts the transaction. */
bool markdown_core_parser_register_definition(markdown_core_parser *parser,
                                              markdown_core_definition_collection *collection,
                                              markdown_core_node *definition);

/* THE LONGEST SOURCE A PARSE TAKES: offsets are int32, and every buffer
 * derived from the source stays under half of that. The public parse entry
 * (markdown_core_document_parse_in) refuses a longer one; below it, `length`
 * is within this bound. */
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
/* One parse transaction: reads `source` as what `revision` says the parse
 * continues, with the storage it lends (above), and returns the published
 * tree, or NULL when the transaction fails. The transaction's state is
 * released before it returns, so an instance runs any number of them, one
 * at a time, each as the first. */
markdown_core_node *markdown_core_parser_parse(markdown_core_parser *parser, const char *source, size_t length,
                                               markdown_core_revision *revision);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
