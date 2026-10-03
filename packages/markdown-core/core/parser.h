#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <assert.h>
#include <stdint.h>
#include "references.h"
#include "node.h"
#include "iterator.h"
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
 * the last id issued. A fresh parse continues nothing: `previous` is NULL
 * and `last_id` is 0, so its nodes are numbered from 1: each owner numbers
 * the nodes it holds when it completes, and the root numbers itself when it completes.
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
    /* Each definition and where it starts, recorded when it opens. */
    struct markdown_core_definition_start {
        struct markdown_core_node *node;
        uint64_t start;
    } *values;
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

/* THE TEXT A PARSE READS (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.1): `size` bytes in pieces, in source order, read a piece at a time as
 * tree-sitter's lexer reads its input. `read` answers the piece that holds
 * byte `offset`, which is below `size`: its first byte, with the offsets
 * where it begins and ends. A buffer is one piece. */
typedef struct markdown_core_text {
    const unsigned char *(*read)(const struct markdown_core_text *text, size_t offset, size_t *start, size_t *end);
    const void *bytes;
    size_t size;
} markdown_core_text;

static inline const unsigned char *markdown_core_text_buffer_read(const markdown_core_text *text, size_t offset,
                                                                  size_t *start, size_t *end) {
    (void)offset;
    *start = 0;
    *end = text->size;
    return (const unsigned char *)text->bytes;
}

/* `size` bytes at `bytes`, as one piece. */
static inline markdown_core_text markdown_core_text_buffer(const void *bytes, size_t size) {
    markdown_core_text text = {markdown_core_text_buffer_read, bytes, size};
    return text;
}

struct markdown_core_parser {
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    markdown_core_source_order source_order;
    /* The stack the inline stage's root passes and then publishing borrow in
     * turn: each grows it to what it needs (markdown_core_parser_walk_stack)
     * and leaves it to the next, and the parse releases it when it ends. */
    void *walk_stack;
    size_t walk_stack_size;
    /* The inline root pass whose node the finish steps are being asked
     * about, or NULL while blocks complete. */
    markdown_core_iter *walk;
    /* THE INLINE ROOTS (docs/plans/2026-09-29-incremental-parsing.md, 5.8):
     * every closed block whose content is inline, and every inline field of a
     * block, with the node that holds it as a child (NULL for a field), the
     * node that owns it, and where each of the two starts, recorded while
     * they held their places. The blocks add them as they close, and the
     * inline stage parses and completes each one, in the order they were
     * added; the queue only borrows the nodes. Its entries are slots of the
     * scratch slabs, back in `inline_root_slots` once the stage has taken
     * them. */
    struct markdown_core_inline_root {
        struct markdown_core_node *node, *parent, *owner;
        uint32_t start, owner_start;
        struct markdown_core_inline_root *next;
    } *inline_roots, *inline_roots_last;
    /* The closed paragraph that held only reference definitions, which the
     * deepest open block holds as its last child until that block takes
     * another child or completes (blocks.c, S_drop_definition_paragraph);
     * NULL when there is none. Only the deepest open block can hold one, so
     * there is at most one. */
    struct markdown_core_node *definition_paragraph;
    /* The inline input being read (node.h, markdown_core_bytes), which every
     * node the parser makes holds; NULL outside an inline parse. */
    markdown_core_bytes *bytes;
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
    /* The block kinds open around `block_root` when it was queued, as
     * markdown_core_node_block_kind_bit sets them: the blocks enclosing an
     * input that the spine, which starts at the input, does not hold. */
    uint32_t block_around;
    struct markdown_core_block_input {
        struct markdown_core_node *owner, *holder;
        uint32_t around;
        /* Where the owner starts, recorded while it held its place: it
         * completes from there once its blocks are read. */
        uint32_t start;
    } *block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    /* Geometry and grammar facts for the active immutable input. The driver
     * and lookahead extend one index; a source byte is scanned for line
     * geometry once, whether the input is the document or a mapped cell. */
    markdown_core_text input_text;
    size_t input_scanned;
    /* The piece of the input the scan last read: its bytes and the offsets
     * where it begins and ends. */
    const unsigned char *input_piece;
    size_t input_piece_start, input_piece_end;
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
    /* A complete candidate may consume through a later source line. The
     * source driver advances past it after the current line has finished. */
    bool claimed;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* THE PARSER'S PATH (docs/plans/2026-09-29-incremental-parsing.md,
     * 5.11): a node has no parent link, so every walk of the parse carries
     * its path here, one frame per node from its root down. While lines are
     * read its frames are the OPEN SPINE: the blocks open at this point of
     * the parse, from `block_root` down to the deepest one, each the parent
     * of the next. A block joins the spine when it is added to its parent and
     * leaves it when it is finalized, and only the deepest block is ever
     * finalized. A tree walk (iterator.h) puts its frames above those already
     * here and takes them off when it is done: an inline field's walk above
     * the spine while lines are read, the inline root passes once they all
     * are. */
    markdown_core_iter_path path;
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
    /* Every node a parse makes, counted at the same operation that records
     * the node's kind, and every node it releases through
     * `markdown_core_parser_release_node`, descendants and field roots
     * included. */
    size_t nodes_created;
    size_t nodes_freed;
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
     * cursor to the offset of the next raw line before it hands each line to
     * `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). The input's size
     * before the first line is processed and once the input has run out. */
    size_t lookahead_cursor;
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
    /* The slots of the delimiter entries of inline parses (see
     * `markdown_core_inline_push_delimiter_entry`) and of the items of
     * inline runs (delimiter.h): an entry or item removed goes back for the
     * next, and the slabs they are cut from go with the parser. */
    markdown_core_slabs scratch_slabs;
    markdown_core_slab_pool delimiters;
    markdown_core_slab_pool inline_items;
    markdown_core_slab_pool inline_root_slots;
    /* The workspace every attribute value of the parse is read into before
     * it is laid out (core/attributes.h); released with the parser. */
    markdown_core_attribute_scratch attribute_scratch;
    /* WHICH KINDS THIS PARSE PRODUCED, recorded where they are produced.
     *
     * Every node creation and every `set_kind` that a parse performs writes
     * here, so the gate on a finish step is read from a record rather than
     * gathered by a walk: a step reads it at each event it is asked at
     * (markdown_core_finish_step_entry). See the gate in `S_finish_parse`
     * for what the set over-approximates and why that is sound.
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
 * created; and a step asked at a node holding none of its declared kinds
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

/* A creation records the kind and counts the node (`nodes_created`). */
static inline void markdown_core_parser_note_node(markdown_core_parser *parser, markdown_core_node_type kind) {
    if (parser) {
        markdown_core_node_kind_set_add(&parser->kinds_created, kind);
        parser->nodes_created++;
    }
}

/* A release counts what it freed (`nodes_freed`); a caller with no parse
 * frees as the public function does. */
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
    markdown_core_node *node = markdown_core_node_pool_new(parser ? parser->pool : NULL, type, NULL);
    if (node && parser) {
        node->bytes = markdown_core_bytes_retain(parser->bytes);
    }
    return node;
}

static inline markdown_core_node *markdown_core_parser_make_node_with_ext(markdown_core_parser *parser,
                                                                          markdown_core_node_type type,
                                                                          const markdown_core_element *element) {
    markdown_core_parser_note_node(parser, type);
    markdown_core_node *node = markdown_core_node_pool_new(parser ? parser->pool : NULL, type, element);
    if (node && parser) {
        node->bytes = markdown_core_bytes_retain(parser->bytes);
    }
    return node;
}

static inline bool markdown_core_parser_set_node_kind(markdown_core_parser *parser, markdown_core_node *node,
                                                      markdown_core_node_type kind) {
    markdown_core_parser_note_kind(parser, kind);
    return markdown_core_node_set_kind(node, kind);
}

/* Puts `child` at the end of `parent`'s children, taking the caller's hold,
 * after the caller has established containment. False, with the parse failed
 * and `child` released, when storage runs out. */
static inline bool markdown_core_parser_append(markdown_core_parser *parser, markdown_core_node *parent,
                                               markdown_core_node *child) {
    if (markdown_core_node_append_validated(parser->pool, parent, child)) {
        return true;
    }
    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    markdown_core_parser_release_node(parser, child);
    return false;
}

/* The deepest open block (the spine's last). */
static inline markdown_core_node *markdown_core_parser_current(const markdown_core_parser *parser) {
    return parser->path.frames[parser->path.count - 1].node;
}

/* Whether `container`, an open block, an open block above it, or a block
 * around the active input is of a kind in `kinds`, a set of
 * markdown_core_node_block_kind_bit bits. */
static inline bool markdown_core_parser_open_within(const markdown_core_parser *parser,
                                                    const markdown_core_node *container, uint32_t kinds) {
    if (parser->block_around & kinds) {
        return true;
    }
    size_t at = parser->path.count;
    while (parser->path.frames[--at].node != container) {
        assert(at);
    }
    do {
        if (markdown_core_node_block_kind_bit((markdown_core_node_type)parser->path.frames[at].node->kind) & kinds) {
            return true;
        }
    } while (at--);
    return false;
}

/* The parent of `node`, an open block, or NULL for `block_root`. */
static inline markdown_core_node *markdown_core_parser_open_parent(const markdown_core_parser *parser,
                                                                   const markdown_core_node *node) {
    size_t at = parser->path.count;
    while (parser->path.frames[--at].node != node) {
        assert(at);
    }
    return at ? parser->path.frames[at - 1].node : NULL;
}

/* THE INLINE ROOT PASS'S CURRENT NODE, for the steps it runs: the changes a
 * step may make to its place (iterator.h). The pass the parser is in is
 * `parser->walk`. */
void markdown_core_parser_publish_node(markdown_core_parser *parser, struct markdown_core_node *node,
                                       const struct markdown_core_node *owner);
/* Puts `node`, complete, just before the current node, taking the caller's
 * hold. The walk does not visit it, so it completes here, held by the
 * current node's parent; its maker completed what it holds. False, with the
 * parse failed and `node` released, when storage runs out. */
static inline bool markdown_core_parser_walk_insert_before(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_parser_publish_node(parser, node, markdown_core_iter_parent(parser->walk));
    if (markdown_core_iter_insert_before(parser->walk, parser->pool, node)) {
        return true;
    }
    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    markdown_core_parser_release_node(parser, node);
    return false;
}
/* At the current node's EXIT: takes it out of the tree and releases it;
 * false, with the parse failed, when storage runs out. */
static inline bool markdown_core_parser_walk_release(markdown_core_parser *parser) {
    markdown_core_node *node;
    if (!markdown_core_iter_take_current(parser->walk, parser->pool, &node)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    markdown_core_parser_release_node(parser, node);
    return true;
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
} markdown_core_input_line;

typedef struct markdown_core_line_facts {
    struct markdown_core_normalized_line *normalized;
    /* The bytes of a line that spans pieces of the input, through its
     * terminator: a view the parse keeps while the input is active (5.1).
     * NULL for a line in one piece, which is read from that piece. */
    const unsigned char *view;
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
     * after the run, so a later scan whose extra containers
     * accept every blank line steps over the run at once. 0 when not a run. */
    int run_end;
    /* Only NUL-bearing lines need this count; it occupies former padding. */
    uint32_t nul_count;
} markdown_core_line_facts;
/* The index is a contiguous prefix. Its next record already owns this line's
 * continuation; at the frontier the scanner owns it. No newline bytes need
 * rereading and no third offset needs retaining on every physical line. */
static inline size_t markdown_core_input_line_next(const markdown_core_parser *parser,
                                                   const markdown_core_input_line *line) {
    const markdown_core_input_line *next = line + 1;
    return next < parser->input_lines + parser->input_line_count ? next->start : parser->input_scanned;
}

/* The byte at `offset` of the active input, which is below its size: in the
 * piece the parse last read when that piece holds it, else in the piece the
 * input reads for it, which the parse then holds. */
static inline const unsigned char *markdown_core_parser_input_at(markdown_core_parser *parser, size_t offset) {
    if (offset < parser->input_piece_start || offset >= parser->input_piece_end) {
        parser->input_piece =
            parser->input_text.read(&parser->input_text, offset, &parser->input_piece_start, &parser->input_piece_end);
    }
    return parser->input_piece + (offset - parser->input_piece_start);
}

/* A line's raw bytes through its terminator. */
static inline const unsigned char *markdown_core_parser_line_bytes(markdown_core_parser *parser,
                                                                   const markdown_core_input_line *line) {
    if (line->facts && parser->input_facts[line->facts - 1].view) {
        return parser->input_facts[line->facts - 1].view;
    }
    return markdown_core_parser_input_at(parser, line->start);
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
    size_t cursor;
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

/* A NODE IS COMPLETE WHEN IT IS MADE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.8). Completing `node`, a block the parse has finished
 * making, drops the paragraph of only definitions still standing as its last
 * child; then a node whose content is inline joins the inline roots, and
 * completes when the root pass has completed its content, while any other
 * node runs the steps declared at its kind's EXIT and completes now. The
 * document completes it (element.h, `complete_node`): the nodes of its
 * relations take their ids and their extents, so every node it holds is
 * complete before it is, and it takes its own when its owner completes. `parent` holds `node` as a child, or is NULL
 * for the document's root. A block the line machine closes is completed by `markdown_core_block_finalize`; a node an
 * element makes closed (a table's row or cell) is completed by that element once it is made. */
void markdown_core_parser_complete(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent);
/* Completes `node`, a field of `owner` (a definition's term, a callout's
 * title, a caption, a block directive's label, the document's metadata), as
 * above; its steps see no parent. */
void markdown_core_parser_complete_field(markdown_core_parser *parser, markdown_core_node *node,
                                         markdown_core_node *owner);
/* Completes `node`, which a step made complete and puts in the tree, held
 * by `owner` (declared with the walk's operations above). */
/* `field` joined `owner`, which holds its place, after `owner` completed (a
 * caption after its table): it is published in its relation. */
void markdown_core_parser_publish_field(markdown_core_parser *parser, const markdown_core_node *owner,
                                        const markdown_core_node *field);
int markdown_core_order_source_entries(markdown_core_source_order *workspace, void *entries, size_t count,
                                       size_t stride, uint64_t (*key)(const void *));

struct markdown_core_block_reader;
/* Query the ordinary block-start rules before the table slot. Paragraph
 * continuation uses its real interruption rules (notably list starts, type-7
 * HTML and indentation); following lines come from the caller's source view. */
bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_node *parent,
                                          markdown_core_chunk *input, int first, int column, int indent, bool paragraph,
                                          struct markdown_core_block_reader *reader);

/* Finalizes the open blocks below `block`, which is open. */
void markdown_core_parser_finalize_to(markdown_core_parser *parser, struct markdown_core_node *block);
/* Schedule an already owned node's mapped content for the ordinary block
 * parser. No nested parse transaction, document, dialect or C recursion.
 * `owner`, which `holder` holds as a child, completes once its blocks are
 * read, or now when it has none. */
bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner,
                                            markdown_core_node *holder);
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
/* Whether `node` begins on input line `line`, a line of the active input the
 * driver has reached. */
bool markdown_core_parser_starts_on_line(markdown_core_parser *parser, const markdown_core_node *node, int line);
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
/* One parse transaction: reads `text` as what `revision` says the parse
 * continues, with the storage it lends (above), and returns the published
 * tree, or NULL when the transaction fails. The transaction's state is
 * released before it returns, so an instance runs any number of them, one
 * at a time, each as the first. */
markdown_core_node *markdown_core_parser_parse(markdown_core_parser *parser, const markdown_core_text *text,
                                               markdown_core_revision *revision);

/* Bytes [start, end) of the active input in one run: borrowed from the
 * piece that holds them, or a view that lives for the parse. NULL when the
 * view cannot be allocated, which fails the parse. */
const unsigned char *markdown_core_parser_input_view(markdown_core_parser *parser, size_t start, size_t end);

/* Releases the storage of the input's line index and views. */
void markdown_core_parser_release_input(markdown_core_parser *parser);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
