#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <assert.h>
#include <stdint.h>
#include "registry.h"
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
 * coordinates: disjoint, in source order. The caller holds `previous`. Every
 * node the parse makes that continues no node of `previous` takes the next id
 * after `last_id` as its owner completes, the document last; on success
 * `last_id` is the last id issued. A fresh parse continues nothing:
 * `previous` is NULL and `last_id` is 0.
 *
 * `pool` lends the parse every node and resource slot it takes (node.h); it
 * outlives the parse, and its owner disposes it. */
typedef struct markdown_core_revision {
    markdown_core_node_pool *pool;
    markdown_core_node *previous;
    const markdown_core_byte_edit *edits;
    size_t edit_count;
    uint64_t last_id;
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
    /* The source bytes each content byte of a copied run reads, 1, or all
     * of the source a decoded run reads. */
    int source_width;
    /* Source bytes advanced per logical byte: 1 for a copied run, whose
     * content bytes are the source bytes they read, and 0 for a decoded
     * run, all of whose content is decoded from its source -- one tab's
     * columns, one NUL's U+FFFD, one escaped pipe's pipe or one line
     * ending's LF. */
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

/* A NODE AND THE SOURCE BYTE IT BEGINS AT, read as the node registers, while
 * its place is still in the source: once it is numbered, its place is its
 * extent (5.8). A collection of them orders by source with
 * markdown_core_source_key. */
typedef struct {
    struct markdown_core_node *node;
    uint32_t start;
} markdown_core_source_entry;

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

/* THE INPUT A PARSE READS (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.1), as tree-sitter's TSInput: `read` returns the input's bytes from
 * `offset`, which is less than `size`, to the end of the chunk that holds
 * them, with their count; `payload` is what it reads. The chunks stay put
 * for the parse. A session reads its text tree a piece at a time; a buffer
 * is one chunk. */
typedef struct markdown_core_input {
    const unsigned char *(*read)(const struct markdown_core_input *input, size_t offset, size_t *size);
    const void *payload;
    size_t size;
} markdown_core_input;

/* A chunk the parse has read: where it starts in the input, its bytes and
 * how many. */
typedef struct markdown_core_input_chunk {
    size_t start, size;
    const unsigned char *bytes;
} markdown_core_input_chunk;

/* The input that reads `size` bytes at `bytes` as one chunk. */
markdown_core_input markdown_core_input_buffer(const unsigned char *bytes, size_t size);

/* AN INLINE ROOT WAITING FOR ITS PARSE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.8): the node whose first relation is the content and whose
 * runs read it, and its member, which waits on the root; the node the content
 * is parsed into, the node itself or the private node its title or term hangs
 * from, and the builder the content is parsed into, that node's member;
 * where the node lies in the source, recorded when it was numbered, since a
 * numbered node holds only its extent; and whether the holder is a field,
 * which belongs to its owner and is never replaced. */
typedef struct markdown_core_inline_root {
    struct markdown_core_node *node, *holder;
    struct markdown_core_member *member, *builder;
    markdown_core_place place;
    bool field;
} markdown_core_inline_root;

/* A CELL WHOSE CONTENT IS READ AS BLOCKS once the document's own lines are:
 * its member, which waits on the input and builds its blocks, and where the
 * cell starts, recorded when it was queued. */
typedef struct markdown_core_block_input {
    struct markdown_core_member *cell;
    uint32_t start;
} markdown_core_block_input;

struct markdown_core_parser {
    /* The registry of the revision's pool (registry.h), which the facts of
     * the nodes the parse makes join, and the node of the inline root whose
     * content is being parsed, which asks it every question its runs ask. */
    markdown_core_registry *registry;
    struct markdown_core_node *asker;
    markdown_core_source_order source_order;
    /* The stack each inline root's completion borrows in turn, growing it
     * to what it needs (markdown_core_parser_walk_stack) and leaving it to
     * the next; the parse releases it when it ends. */
    void *walk_stack;
    size_t walk_stack_size;
    /* THE INLINE ROOTS, in the order the nodes holding them were numbered
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.8): each is parsed
     * and completes its own tree once every block is complete and the
     * document is prepared. */
    struct markdown_core_inline_root *inline_roots;
    size_t inline_root_count, inline_root_capacity;
    /* The root whose content is being completed, or NULL: the nodes numbered
     * meanwhile lie in its content. */
    struct markdown_core_inline_root *completing;
    /* The last id issued: a node takes the next when its owner completes. */
    uint64_t last_id;
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
    /* The builder of the document (node.h, markdown_core_member), which
     * holds the document's reference until the parse returns it. */
    struct markdown_core_member *root;
    /* The active block grammar boundary: the document's builder, or a mapped
     * cell's while its content is read. The document and mapped cell inputs
     * share this parser and all document registries. Input roots stay owned by
     * the AST; the queue only borrows them until their block content is read. */
    struct markdown_core_member *block_root;
    struct markdown_core_member *matched_container;
    struct markdown_core_block_input *block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    /* Geometry and grammar facts for the active immutable input. The driver
     * and lookahead extend one index; a source byte is scanned for line
     * geometry once, whether the input is the document or a mapped cell. The
     * scan reads the input a chunk at a time, each chunk once: `input_chunks`
     * holds the chunks read so far, in order of their starts, which cover
     * what the parse read of the input and leave out what it took without
     * reading (5.3), and `input_chunk` is the one read last or asked for last,
     * holding `input_chunk_size` bytes from `input_chunk_start` on. */
    markdown_core_input input;
    struct markdown_core_input_chunk *input_chunks;
    size_t input_chunk_count, input_chunk_capacity;
    const unsigned char *input_chunk;
    size_t input_chunk_start, input_chunk_size;
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
    /* A complete candidate may consume through a later line, `claimed_line`.
     * The source driver advances to it after the current line has finished. */
    bool claimed;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* THE PARSE'S READING OF ITS OLD TREE (docs/plans/2026-09-29-incremental-
     * parsing.md, 5.1-5.3). `line_reach` is the high-water mark of the line
     * being processed: the end of the furthest line any decision on it read,
     * through its terminator, as markdown_core_parser_source_line raises it.
     * `line_context` says whether the line was read with an open paragraph
     * as its context: a start the line would open after a closed block was
     * refused (markdown_core_block_start_refuses), and `previous_blank` whether the line before it was blank past
     * its prefixes: a block closed by such a line trails
     * (MARKDOWN_CORE_NODE__TRAILED). `edit_shift` is the length change
     * before each of the revision's edits. A take (markdown_core_parser_add_
     * child) sets `taken`: the line ends there, and the next one read begins
     * at `resume`, after a line whose content ended at `resume_last_end`;
     * `resume_blank` says that line was blank, and `resume_flags` are the
     * blank-line flags the innermost open block had there (5.3). */
    uint32_t line_reach;
    bool line_context, previous_blank, taken;
    /* The line in hand as a leaf's line (E5): `plain` holds where its
     * content began when it continued the current leaf as a plain line, and
     * `lines_taken` says that the line began a run of lines a leaf took. */
    markdown_core_line plain;
    bool lines_taken;
    int64_t *edit_shift;
    size_t resume;
    bufsize_t resume_last_end;
    bool resume_blank;
    uint16_t resume_flags;
    /* THE NODES OF TAKEN SUBTREES THE PARSE REPLACES (5.7), in the order
     * found: a root whose lookups are answered otherwise now, parsed again
     * as `member`, whose node takes its place once it settles, and a heading
     * whose anchor changes, whose copy `node` takes its place. `start` is
     * where the old node begins. The document puts each in its place as it
     * finishes (markdown_core_publication_splice). */
    struct markdown_core_replacement {
        const struct markdown_core_node *old;
        struct markdown_core_node *node;
        struct markdown_core_member *member;
    } *replacements;
    size_t replacement_count, replacement_capacity;
    /* The last open block after a line is fully processed */
    struct markdown_core_member *current;
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
    /* The input's last line as the block parser will see it, normalized once
     * and reused by every lookahead that reaches it: it has no terminator of
     * its own in the source, and a line handed to the prefix matchers must
     * end in one. */
    markdown_core_strbuf lookahead_last_line;
    bool lookahead_last_line_ready;
    /* The open containers a lookahead matches, root first, with the flags they
     * had when it began; and the per-line resume cache. Both are owned by the
     * parser so a document with many candidates allocates them once. */
    struct markdown_core_member **lookahead_chain;
    markdown_core_node_internal_flags *lookahead_chain_flags;
    int lookahead_chain_alloc;
    /* Delimiter entries removed from an inline parse, kept for the next push
     * (see `markdown_core_inline_push_delimiter_entry`); linked through `next`
     * and released with the parser. */
    struct delimiter *free_delimiters;
    /* Where each delimiter the run of an inline root's content pushed left
     * the stack: the content offset of the closer whose reduction removed
     * it, or INT32_MAX for one that stayed to the end of the content
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.6). A delimiter's
     * `stay` is its index here plus one; the run starts the list again and it
     * is released with the parser. */
    int32_t *stays;
    size_t stay_count, stay_capacity;
    /* The workspace every attribute value of the parse is read into before
     * it is laid out (core/attributes.h); released with the parser. */
    markdown_core_attribute_scratch attribute_scratch;
    /* WHICH KINDS THIS PARSE PRODUCED, recorded where they are produced.
     *
     * Every node creation and every `set_kind` that a parse performs writes
     * here, so the gate on a completion step is read from a record rather
     * than gathered by a walk: a step reads it at each event it is asked at
     * (markdown_core_complete_step_entry). See
     * `markdown_core_parser_note_kind` for what the set over-approximates and
     * why that is sound.
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
 * `kinds_created` decides which completion steps run at the events they were
 * projected to, so a production site that writes a kind
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
 * steps, never miss one, because a kind in the finished tree was necessarily
 * created; and a step asked at a tree holding none of its declared kinds
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

/* A release gives the node's slots back to the parse's pool; a caller with
 * no parse frees as the public function does. */
static inline void markdown_core_parser_release_node(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_node_pool_release(parser ? parser->pool : NULL, node);
}

/* A member for `node` from the parse's pool (node.h); NULL, with the parse
 * failed, when it could not be allocated. */
static inline markdown_core_member *markdown_core_parser_member(markdown_core_parser *parser, markdown_core_node *node,
                                                                bool held) {
    markdown_core_member *member = markdown_core_member_new(parser->pool, node, held);
    if (!member) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return member;
}

/* THE PARSE'S DECLARATIONS (registry.h). `node` declares `label`, already in
 * its normal form, in `group`: a Reference its label, a definition its own.
 * An empty label declares nothing. The parse fails when the fact could not
 * be made. */
static inline void markdown_core_parser_declare(markdown_core_parser *parser, markdown_core_node *node,
                                                markdown_core_key_group group, const markdown_core_chunk *label) {
    if (label->len > 0 && !markdown_core_registry_declare(parser->registry, node, group, MARKDOWN_CORE_FACT_DECLARE,
                                                          label->data, (uint32_t)label->len, NULL, 0)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* `label` in its normal form, in storage from the parse's pool the node that
 * declares it owns (markdown_core_node_pool_bytes), NUL-terminated; empty when
 * it normalizes to nothing. False when storage could not be had. */
bool markdown_core_parser_normalize_label(markdown_core_parser *parser, const markdown_core_chunk *label,
                                          markdown_core_chunk *normalized);

/* Whether the pass asks `entry`'s step at the event it is projected to: its
 * gate, read against the kinds the parse has produced so far. */
static inline bool markdown_core_complete_step_admitted(const markdown_core_complete_step_entry *entry,
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

/* A block its lines turned into another kind reads the old node of that
 * kind which begins where it does (blocks.c). */
void markdown_core_parser_kind_changed(markdown_core_parser *parser, markdown_core_member *member);

/* Turns the block `member` holds into one of `kind`, as its lines decided. */
static inline markdown_core_node_set_kind_result markdown_core_parser_set_node_kind(markdown_core_parser *parser,
                                                                                    markdown_core_member *member,
                                                                                    markdown_core_node_type kind) {
    markdown_core_parser_note_kind(parser, kind);
    const markdown_core_node_type was = (markdown_core_node_type)member->node->kind;
    markdown_core_node_set_kind_result result =
        markdown_core_node_set_kind(member->node, markdown_core_parser_owner(parser, member), kind);
    if (member->node->kind != was) {
        markdown_core_parser_kind_changed(parser, member);
    }
    return result;
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
     * lookahead last matched them (markdown_core_parser_place_runs). */
    uint32_t own;
} markdown_core_input_line;

typedef struct markdown_core_line_facts {
    struct markdown_core_normalized_line *normalized;
    /* Table grammar search facts under one matched container prefix. */
    const struct markdown_core_member *table_container;
    int table_offset;
    unsigned table_absent;
    /* A candidate matches open-container prefixes on every line it visits.
     * Failed candidates reaching the same line have nested container chains,
     * so each resumes from the deepest match already recorded. Each
     * (container, line) prefix is matched at most once per parse. NULL means
     * that no scan has recorded a prefix. */
    const struct markdown_core_member *container;
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
     * after the run, so a later scan whose extra containers accept every
     * blank line steps over the run at once. 0 when not a run. */
    int run_end;
    /* Only NUL-bearing lines need this count; it occupies former padding. */
    uint32_t nul_count;
    /* The bytes of a line that spans chunks, terminator included, joined into
     * one view the first time they are read (blocks.c). */
    const unsigned char *joined;
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

/* THE EDIT MAPPING (docs/plans/2026-09-29-incremental-parsing.md, 5.2) of
 * a parse that continues a tree. `markdown_core_parser_edit_after` is the
 * first edit that ends after old byte `x`: every one before it ends at or
 * before x. `markdown_core_parser_image` is where the boundary at old offset
 * `x` lies now: inside a replaced range it lies where the replacement ends.
 * `markdown_core_parser_edge` is where the first edit that ends after old
 * byte `from` begins, INT64_MAX when none does, and
 * `markdown_core_parser_touched` says whether an edit meets or touches the
 * old range [from, to], both ends included. `markdown_core_parser_source_
 * anchor` gives the image of the first byte of the old range [start, end)
 * that no edit replaced, false when every byte of it was replaced. */
size_t markdown_core_parser_edit_after(const markdown_core_parser *parser, uint32_t x);
uint32_t markdown_core_parser_image(const markdown_core_parser *parser, uint32_t x);
/* The old offset whose image is `y`, a byte no edit replaced: the later one
 * when an edit removed bytes there. */
uint32_t markdown_core_parser_origin(const markdown_core_parser *parser, uint32_t y);
int64_t markdown_core_parser_edge(const markdown_core_parser *parser, uint32_t from);
bool markdown_core_parser_touched(const markdown_core_parser *parser, uint32_t from, uint32_t to);
bool markdown_core_parser_source_anchor(const markdown_core_parser *parser, uint32_t start, uint32_t end,
                                        uint32_t *image);

/* `old`, a node of a subtree the parse took, gives its place to `node`, or
 * to the node of `member` once it settles, which holds `old`'s order; the
 * replacement takes the reference to either. False, with the parse failed
 * and the reference released, when the list could not grow. */
bool markdown_core_parser_replace(markdown_core_parser *parser, const markdown_core_node *old, markdown_core_node *node,
                                  markdown_core_member *member);

/* THE STATE `parent` CARRIES where a child of it begins after `previous`
 * (5.3, E3): the word its element saves, the kind of the child before, and
 * its blank-line flags. A block records it as its `entry`. */
uint64_t markdown_core_parser_carry(const markdown_core_parser *parser, const markdown_core_member *parent,
                                    const markdown_core_node *previous);

/* A BLOCK READ OFF THE FRONT OF `whole` (5.3): a Reference a paragraph's
 * definition gives, or the lead paragraph a table splits off. `piece` is
 * attached before `whole`. It begins where `whole` began, so it takes its
 * entry and its reach, and it holds the next block, the rest of `whole`,
 * which begins after it with the state its parent carries there. NULL when
 * it could not be attached. */
markdown_core_member *markdown_core_parser_attach_split(markdown_core_parser *parser, markdown_core_member *whole,
                                                        markdown_core_node *piece);

/* A LATER LINE WRITES INTO THE CLOSED LAST CHILD OF `parent` (5.4, E2), a
 * separate-line block identifier or a table's trailing caption: the child
 * ends at `end`, its reach and its parent's cover what the line has read,
 * and it holds the next block, so that no run of taken blocks ends at it. */
void markdown_core_parser_write_closed(markdown_core_parser *parser, markdown_core_member *parent, bufsize_t end);

/* Line `line` of the input, read: a decision that reads a line reads it
 * whole, through its terminator, which raises the high-water mark of the
 * line being processed (parser.h, `line_reach`). */
static inline markdown_core_input_line *markdown_core_parser_source_line(markdown_core_parser *parser, int line) {
    if (line < parser->input_first_line || parser->error) {
        return NULL;
    }
    size_t index = (size_t)(line - parser->input_first_line);
    markdown_core_input_line *read = index < parser->input_line_count
                                         ? &parser->input_lines[index]
                                         : markdown_core_parser_extend_source_lines(parser, index);
    if (read) {
        const uint32_t next = (uint32_t)markdown_core_input_line_next(parser, read);
        parser->line_reach = next > parser->line_reach ? next : parser->line_reach;
    }
    return read;
}

/* The bytes of `line` through its terminator, contiguous: borrowed from the
 * input's chunk, or, for a line that spans chunks, joined once for the input
 * and kept in the line's facts. NULL, with the parse failed, when an
 * allocation failed. */
const unsigned char *markdown_core_parser_line_bytes(markdown_core_parser *parser, markdown_core_input_line *line);

/* The bytes of input lines `first` to `last`, terminators included, as one
 * view: borrowed when they lie in one chunk, else joined for the input. NULL,
 * with the parse failed, when the view could not be allocated. */
const unsigned char *markdown_core_parser_input_view(markdown_core_parser *parser, int first, int last);

/* Whether the input holds a line after the line being processed: a block
 * start whose grammar needs a later line looks ahead only when it does. */
static inline bool markdown_core_parser_input_continues(markdown_core_parser *parser) {
    return markdown_core_parser_source_line(parser, parser->line_number + 1) != NULL;
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
    struct markdown_core_member *parent;
    int depth;
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
bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_member *parent,
                                          markdown_core_chunk *input, int first, int column, int indent, bool paragraph,
                                          struct markdown_core_block_reader *reader);

/* Schedule an already owned node's mapped content for the ordinary block
 * parser. No nested parse transaction, document, dialect or C recursion. */
void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser);
/* Queues the content of the cell `member` builds, which waits on it. */
bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_member *member);
/* Whether `node` holds inline content (its kind's record, dialect.h). */
bool markdown_core_parser_contains_inlines(markdown_core_parser *parser, markdown_core_node *node);
/* Puts the inline root `member`'s node holds on the parser's list
 * (markdown_core_inline_root), and the member waits on it. Its builder is
 * the member, or one made for `holder` as a field root of the member's. False
 * when the list or the builder could not be allocated. */
bool markdown_core_parser_hold_inline_root(markdown_core_parser *parser, markdown_core_member *member,
                                           markdown_core_node *holder, markdown_core_place place, bool field);
/* `member` waits on one thing less -- an inline root, a block input, a value
 * an element gives it late -- and settles when it was numbered and waits on
 * nothing more (docs/plans/2026-09-29-incremental-parsing.md, 5.9). An
 * element that gives a numbered node a value late makes its member wait by
 * counting one more in `waits` first. */
void markdown_core_parser_release_wait(markdown_core_parser *parser, markdown_core_member *member);
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
/* THE RUNS OF THE OWN LINES OF `node`, a block of `container`, which starts
 * on input line `*line`, an earlier one or a later one of the lines visited:
 * on each line its place spans, a run that decodes nothing over the source from where
 * the line's own bytes begin to where the line ends, through its terminator
 * on a line of the document itself, those that touch joined. A node whose
 * lines join into one run keeps none, and a block of the document itself,
 * which lies on whole lines of the source, has none. The runs hold absolute
 * source ranges, which reading the node's content fills with its content
 * runs (markdown_core_parser_read_content) and publishing clips to its place
 * (node.h). `*line` becomes the line the node ends on, where a later node can
 * start the search. */
void markdown_core_parser_place_runs(markdown_core_parser *parser, markdown_core_node *node,
                                     const markdown_core_member *container, int *line);
/* Whether `node` begins on input line `line`, a line of the active input the
 * driver has reached. */
bool markdown_core_parser_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
/* An inline root's content, `length` bytes of it read, becomes the input its
 * nodes are placed in: its map to the source is kept in its runs, among the
 * runs that decode nothing of its own source, and its map becomes the
 * identity (blocks.c). */
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

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, struct markdown_core_member *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead);
/* The next non-blank line that carries the prefixes: its bytes through its
 * line ending, the index of its first non-space byte, its indentation after
 * the prefixes, and the blank lines stepped over before it. Returns 0 when the
 * lookahead has ended. */
int markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, markdown_core_chunk *line,
                                        int *first_nonspace, int *indent, int *blank_lines);
void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead);

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
/* One parse transaction: reads `input` as what `revision` says the parse
 * continues, with the storage it lends (above), and returns the published
 * tree, or NULL when the transaction fails. The transaction's state is
 * released before it returns, so an instance runs any number of them, one
 * at a time, each as the first. */
markdown_core_node *markdown_core_parser_parse(markdown_core_parser *parser, const markdown_core_input *input,
                                               markdown_core_revision *revision);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
