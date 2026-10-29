#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <assert.h>
#include <stdint.h>
#include "array.h"
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

/* WHAT A PARSE CONTINUES: the storage it takes nodes from and the ids it
 * issued.
 *
 * A session's parse publishes the tree it builds, in place, at each of its
 * snapshots and at its finalize, and each publish continues the one before
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.9): a node a publish
 * placed keeps its id, a node matched to a node the last publish placed
 * takes its id, and every other node takes the next id after `last_id`,
 * which is then the last id issued. A fresh parse continues nothing: `last_id`
 * is 0, so its root is 1 and every node is numbered in canonical walk order.
 *
 * `pool` lends the parse every node and resource slot it takes (node.h); it
 * outlives the parse, and its owner disposes it. */
typedef struct markdown_core_revision {
    markdown_core_node_pool *pool;
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
     * or with `\r` and another byte after it. Asking for a line that starts
     * at or past it reads the end of the input
     * (markdown_core_parser_extend_source_lines). A cell's content is
     * complete when it is read, so this is its end. */
    size_t complete;
    MARKDOWN_CORE_ARRAY(struct markdown_core_input_line) lines;
    struct markdown_core_normalized_line *normalized;
    struct markdown_core_line_facts *facts;
    size_t fact_count, fact_capacity;
    /* Whether a column of the input may stand elsewhere than the same
     * offset from its line's start in the source: true for a cell's content,
     * and for the document once its scan finds NUL, which the block parser
     * reads as the three bytes of U+FFFD. */
    bool mapped;
    int first_line;
    /* The input's epoch: a number the parser gives each input as it installs
     * it, and the document's text each time it grows (blocks.c,
     * S_parse_source), so a line of one epoch has the same bytes whenever it
     * is read. */
    size_t epoch;
    /* The input's last line as the block parser will see it, normalized once
     * and reused by every lookahead that reaches it: it has no terminator of
     * its own in the source, and a line handed to the prefix matchers must
     * end in one. */
    markdown_core_strbuf last_line;
    bool last_line_ready;
} markdown_core_parser_input;

/* A BLOCK A SNAPSHOT OR A LINE'S MARK LENT (blocks.c, THE LENDING, THE
 * CANDIDATES): the block's address, which holds the view of it, the slot
 * that holds the block's own state, its last child as the lending found it,
 * and whether the view left its holder's children as it closed, yielding no
 * node of its own. */
typedef struct markdown_core_lend {
    struct markdown_core_node *address, *backup, *last;
    bool dropped;
} markdown_core_lend;

/* A HOLDER of the first line's mark (blocks.c, THE CANDIDATES; parser.h,
 * THE DECIDED CONTENT): a block the mark found open or pending, which it
 * lent, how many children it held and the last of them, the slot that
 * holds its state as the mark found it, with the fields an accept keeps,
 * and its depth among the blocks the mark lent, the root's 0 and the
 * pending block's one past the current block's. */
typedef struct markdown_core_holder {
    const struct markdown_core_node *node, *last, *backup;
    uint32_t count, depth;
} markdown_core_holder;

/* A ROOM: the open block below the blocks a snapshot lent that the lending
 * reached, as the lending found it (blocks.c, THE LENDING): how many
 * children it held, the last of them, its flags, where it ended and its
 * anchor, which the snapshot writes in place as the blocks it lent settle
 * into it. */
typedef struct markdown_core_room {
    struct markdown_core_node *node, *last;
    uint32_t count, end;
    markdown_core_node_internal_flags flags;
    markdown_core_chunk anchor;
} markdown_core_room;

/* A RECORD A PUBLISH KEPT (ast.c, THE RECORDS): a node of a relation of a
 * node the publish walked in place that was not FINAL: its id and kind,
 * where its published source lay, the owner's relation it was in, the next
 * record of the same owner, plus one, or 0, and the first record the publish
 * kept of the node's own relations, plus one, or 0. A node a snapshot made
 * is the record's `node`, which the record owns once the next take-back
 * unhooks it (blocks.c, THE LENDING); a node that stays in the tree, which
 * the commits keep or release, is no record's node. A free record is linked
 * through `next`. */
typedef struct markdown_core_tail_record {
    struct markdown_core_node *node;
    uint64_t id;
    uint32_t start, end, field, next, held;
    uint16_t kind;
} markdown_core_tail_record;

/* The records the publishes keep, the root's first: its `held` is the first
 * record of the root's relations. The free ones are linked from `free`,
 * plus one, or 0. */
typedef struct markdown_core_tail_records {
    markdown_core_tail_record *items;
    size_t count, capacity;
    uint32_t free;
} markdown_core_tail_records;

/* The nodes `records` owns, released, and the records emptied. */
void markdown_core_parser_release_records(markdown_core_parser *parser, markdown_core_tail_records *records);

/* The `count` top-level nodes `nodes` of the content of the paragraph
 * `root`, which the last publish placed, leave it while it goes on (blocks.c,
 * THE SETTLE POINT): each one that publish placed becomes an old node of the
 * content that publish kept a record of, owned by its record (ast.c, THE
 * RECORDS), and the next publish matches the new content against them. The
 * others, and all of them when that publish kept no record of `root`, are
 * released. False, with them all released, when the records could not
 * grow. */
bool markdown_core_publish_keep_left(markdown_core_parser *parser, const markdown_core_node *root,
                                     markdown_core_node *const *nodes, uint32_t count);

/* THE COMMITTED TABLES (ast.c, THE DEFINITION TABLES A STREAMING PARSE
 * KEEPS): the definitions the published document holds as COMMITTED. Each
 * of the document's tables -- Footnotes, Specimens, References, Headings --
 * by where they start, an entry's number its start and its pointer the
 * object the document holds; and the labeled Footnotes, the labeled
 * Specimens and the reference targets by label, an entry's pointer the
 * label's definition and its number where that starts. */
enum { MARKDOWN_CORE_DEFINITION_TABLES = 4, MARKDOWN_CORE_LABEL_TABLES = 3 };

typedef struct markdown_core_committed_tables {
    markdown_core_key_tree starts[MARKDOWN_CORE_DEFINITION_TABLES];
    markdown_core_key_tree labels[MARKDOWN_CORE_LABEL_TABLES];
} markdown_core_committed_tables;

/* A DEFINITION A CANDIDATE LISTS (blocks.c, A LOOKUP THAT FOUND NOTHING):
 * where it starts in the source, its table and the object the document
 * holds. */
typedef struct {
    uint32_t start, table;
    const struct markdown_core_node *node;
} markdown_core_defined;

/* A COMMITTED DEFINITION A PUBLISH LISTS as it walks: its table and its
 * entry there, which name the object the publish places once it is done. */
typedef struct {
    uint32_t table, entry;
} markdown_core_kept;

/* Where an inline root lay as the block phase left it: its place, and its
 * places, which the parser owns until a candidate takes them. */
typedef struct {
    markdown_core_place place;
    markdown_core_places *places;
} markdown_core_miss_position;

/* AN INLINE ROOT A LOOKUP FOUND NOTHING FOR at a commit's completion
 * (markdown_core_parser_find_definition; blocks.c, A LOOKUP THAT FOUND
 * NOTHING): a block whose own inline content asked, or the block that holds
 * the field root that asked (`field`); the block the root settled into, or
 * that holds it; and where the root lay as the block phase left it. */
typedef struct {
    struct markdown_core_node *node, *holder;
    markdown_core_miss_position position;
    bool field;
} markdown_core_miss_root;

/* A definition lookup a commit's completion found nothing for: the root it
 * was made for, among the parser's `miss_roots`, and the lookup's kind and
 * label, as written, at `label_start` in the parser's `miss_labels`. */
typedef struct {
    size_t root;
    markdown_core_registry_kind kind;
    bufsize_t label_start, label_length;
} markdown_core_definition_miss;

/* WHERE A COMMIT STOPPED IN THE LINE A SESSION HOLDS (blocks.c): the step
 * the line machine stands before -- matching the open block below
 * `container`, opening a block in `container`, or adding the rest of the
 * line to `container` as text, with `all_matched` whether the line matched
 * every open block, `opened` whether it has opened one, `maybe_lazy` whether
 * it may still be lazy and `depth` the blocks it has asked to open -- the
 * calls of the line that asked before the step (markdown_core_parser_at,
 * THE ASKS), and the line's cursor there and where its own bytes begin,
 * which the next reading of the line starts from. */
typedef enum {
    MARKDOWN_CORE_LINE_AT_MATCH,
    MARKDOWN_CORE_LINE_AT_OPEN,
    MARKDOWN_CORE_LINE_AT_TEXT,
} markdown_core_line_step;

typedef struct markdown_core_line_stop {
    struct markdown_core_node *container;
    size_t depth, asks;
    bufsize_t offset, column;
    uint32_t own;
    markdown_core_line_step step;
    bool all_matched, opened, maybe_lazy, partially_consumed_tab;
} markdown_core_line_stop;

/* WHAT A STREAMING PARSE KEEPS: every parse, which takes snapshots
 * between its commits (blocks.c, THE SNAPSHOT) and publishes its tree in
 * place. The parser owns it from markdown_core_parser_start until the parse
 * is released. */
/* THE SETTLE POINT OF AN INLINE ROOT (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.8). A root a snapshot reads is parsed from `from`, a token start before
 * which its nodes are its caller's (0: the whole content). The parse lists
 * in `starts` the starts of the top-level tokens it read before any token
 * read the end of the content, where nothing read before holds anything
 * open, each with how many lookups had found nothing before it (`misses`,
 * which the lookups count), and leaves in `undecided` the earliest start of
 * a token whose reading read the end, or of an opener left unmatched,
 * INT32_MAX when there is none. It lists in `notes` the inline notes it
 * made, fields their Citations hold, which commit with the nodes that hold
 * them as those settle. */
typedef struct {
    bufsize_t start;
    uint32_t misses;
} markdown_core_settle_start;

typedef struct markdown_core_inline_settle {
    bufsize_t from, undecided;
    uint32_t misses;
    markdown_core_settle_start *starts;
    size_t count, capacity;
    struct markdown_core_node **notes;
    size_t note_count, note_capacity;
} markdown_core_inline_settle;

/* A lookup a snapshot's reading of an open paragraph found nothing for: its
 * kind and its label, at `label_start` in the settled labels. */
typedef struct {
    markdown_core_registry_kind kind;
    bufsize_t label_start, label_size;
} markdown_core_settled_miss;

/* THE SETTLED NODES OF AN OPEN PARAGRAPH (blocks.c, THE SETTLE POINT): the
 * paragraph at `address` whose id is `id` and kind `kind`, as the last
 * snapshot that completed it read it -- its content from `offset` in its
 * text, `size` bytes of it -- the top-level nodes before its settle point
 * `point`, the `nodes`, which the stream owns, the lookups
 * before the point that found nothing, the first `miss_count` of `misses`,
 * and the nodes below them that hold an explicit anchor. Its content's next read goes on
 * at the mark `mark` of its map, content offset `from`, with the places
 * read before it, which end at `at` in the source. While a snapshot reads
 * the paragraph, `root` is the paragraph, `settle` what its parse lists,
 * and the nodes, misses and labels past the held ones are the parse's: the
 * paragraph's children are the nodes' storage, which the parse writes past
 * their count (array.h, markdown_core_array_reserve_past). While the
 * nodes are `taken`, the commit's completion of the paragraph holds them,
 * and an accept that puts the paragraph back gives them back. */
typedef struct {
    struct markdown_core_node *address, *root;
    uint64_t id;
    uint16_t kind;
    bufsize_t offset, size, point, from;
    MARKDOWN_CORE_ARRAY(struct markdown_core_node *) nodes;
    uint32_t mark, at;
    markdown_core_places *places;
    markdown_core_settled_miss *misses;
    size_t miss_count, miss_total, miss_capacity;
    markdown_core_strbuf labels;
    struct markdown_core_node **anchored;
    size_t anchored_count, anchored_capacity;
    markdown_core_inline_settle settle;
    bool taken;
} markdown_core_settled;

typedef struct markdown_core_stream {
    /* The line the last commit stopped in, when `held`. */
    markdown_core_line_stop stop;
    bool held;
    /* Whether the text of the commit being read is the whole text: the
     * finalize's, which no append follows. */
    bool ends;
    /* THE LENDING of the last snapshot (blocks.c): the blocks it lent, in
     * the order it lent them, the rooms of the open blocks the lending
     * reached, in the order it reached them, and the open block the lending
     * reaches next, NULL once it lent the root. The published document
     * reads the lent blocks until the next take-back. */
    markdown_core_lend *lends;
    size_t lend_count, lend_capacity;
    markdown_core_room *rooms;
    size_t room_count, room_capacity;
    struct markdown_core_node *boundary;
    /* THE RECORDS (ast.c). */
    markdown_core_tail_records records;
    /* The definitions the published document holds as committed (ast.c),
     * room for those a publish lists as it walks, and the definitions the
     * candidates of a finalize list, which join the committed tables once
     * every candidate is placed (blocks.c). */
    markdown_core_committed_tables committed;
    markdown_core_kept *kept;
    size_t kept_capacity;
    /* Room for the entries a snapshot's document lists as its own (ast.c,
     * A SNAPSHOT'S TABLES), which every snapshot of the parse reuses: the
     * document borrows them until the next snapshot. */
    void *own_tables;
    size_t own_table_capacity;
    markdown_core_defined *defined;
    size_t defined_count, defined_capacity;
    /* THE LOOKUPS A COMMIT'S COMPLETION FOUND NOTHING FOR, in the order
     * they were made (plan 5.4), and the inline roots they were made for,
     * the last of which is `miss_root` while its completion lasts, and how
     * many of the roots a snapshot published once they were complete. The
     * finalize asks each lookup again with every definition visible, and
     * completes again each root whose answer is one now (blocks.c, A LOOKUP
     * THAT FOUND NOTHING). */
    markdown_core_definition_miss *misses;
    size_t miss_count, miss_capacity;
    markdown_core_strbuf miss_labels;
    markdown_core_miss_root *miss_roots;
    size_t miss_root_count, miss_root_capacity, miss_published;
    const struct markdown_core_node *miss_root;
    /* THE CANDIDATES (blocks.c): the marks of the lines whose opener calls
     * the text read so far does not decide, outermost first, and how many
     * entries keep storage; the call being decided and its mark; the call a
     * reading meets next, and the depth in its mark's path of the container
     * of the last one it met, and whether an accept put the commit back at
     * the last mark, whose line a reading reads anew; the call a snapshot
     * accepted, which claims in the snapshot's reading anew; the line through
     * which the feed has fed every call; and the request the line being read
     * made of its opener's call; and room for one record a candidate keeps
     * (element.h, `candidate_state_size`), the dialect's largest: the one
     * the request gives, which its call's mark copies, and the copy of a
     * call's that a snapshot feeds (blocks.c, A SNAPSHOT SEES A CANDIDATE
     * END); after it, the room of what the mark of the snapshot under way
     * keeps of the elements' records (blocks.c, THE MARK), the mark being
     * `snapshot_mark` while the snapshot lasts. */
    struct markdown_core_candidate_mark *marks;
    size_t mark_count, mark_capacity, mark_made;
    struct markdown_core_candidate *deciding;
    struct markdown_core_candidate_mark *decision_mark;
    size_t meet_mark, meet_call, meet_depth;
    bool anew;
    struct markdown_core_candidate *claimant;
    int fed;
    const markdown_core_element_instance *request_owner;
    struct markdown_core_node *request_container;
    markdown_core_node_type request_child;
    size_t request_ask;
    bool request_ends, requested;
    struct markdown_core_parser_mark *snapshot_mark;
    /* THE EDIT the next publish follows (blocks.c, THE REOPEN): it replaced
     * the old text from `edit_from` to `edit_to` and moved what follows by
     * `edit_delta`; all zero for every other publish (ast.c, THE IMAGE OF AN
     * OLD ANCHOR). */
    uint32_t edit_from, edit_to;
    int64_t edit_delta;
    /* THE REACH (blocks.c): each reading of the document that decided what
     * starts at `start` from the bytes up to `end` -- a child of the
     * document that closed as the line at `end - 1` was read, a lookahead
     * that read past the line after its own, a candidate a later line
     * rejected -- in the order the readings ran. An edit reads from no
     * point such a reading spans. */
    markdown_core_place *reach;
    size_t reach_count, reach_capacity;
    /* What the edits that read the parse again after its flush keep: a
     * session's (markdown_core_parser_edits), which keeps the reach, the
     * headings' state and the old tail of each edit (THE SPLICE); NULL for
     * a one-shot parse, which keeps none of them. */
    struct markdown_core_edit *edit;
    /* THE SETTLE POINT (blocks.c). */
    markdown_core_settled settled;
    markdown_core_state_alignment candidate_room[];
} markdown_core_stream;

/* WHERE A READING STANDS: every field of the parser a reading of lines
 * writes as it goes, one value, which what puts a reading back -- a
 * snapshot (blocks.c, THE SNAPSHOT), a candidate's decision
 * (markdown_core_parser_candidate_start), a lookahead
 * (markdown_core_parser_lookahead_begin) and the cells' inputs (blocks.c,
 * S_parse_block_inputs) -- keeps and puts back whole. The fields with a
 * getter in markdown-core-element-api.h are documented there. */
typedef struct markdown_core_parser_at {
    /* THE BUILDERS (docs/plans/2026-09-29-incremental-parsing.md, 5.2): the
     * blocks open in the active input, from its root to the deepest, the
     * current block. Each is the last of the children of the one before it,
     * which it joins as it opens (blocks.c). They are a stack linked through
     * their scaffolds (node.h, THE SCAFFOLD): a builder's `parent` is the
     * builder it is open inside.
     * `block_root` is the active input's root -- the document, or a table
     * cell whose content is the input -- which the input's end closes in
     * place, and `current` the current block. `pending` is the block that
     * closed last, the last child of the current block, until it settles
     * (A BLOCK SETTLES): the lines
     * after its close still write it -- a blank line its LAST_LINE_BLANK, a
     * block identifier its anchor and end, a caption its table -- and nothing
     * writes a settled block but completion, which makes its inline
     * content. The document and the cell inputs share this parser and all
     * document registries. Input roots stay owned by the AST; the queue only
     * borrows them until their block content is read. */
    struct markdown_core_node *block_root, *current, *pending;
    /* The line being read, its input line number, and where on it the
     * reading stands: the byte offset and the column, the first non-space
     * byte and its column, the indentation between them, whether the line is
     * blank, whether a tab the offset is inside is partly consumed, and
     * where a thematic break failed on it. */
    int line_number, indent;
    bufsize_t offset, column, first_nonspace, first_nonspace_column, thematic_break_kill_pos;
    bool blank, partially_consumed_tab;
    /* Where input line `line_number` starts in the active input and where it
     * ends, before its line ending: the driver records both as it hands the
     * line to the block parser, they hold while `curline` does, and a claim
     * of later lines moves the start. Where the last line read ended. */
    bufsize_t line_start, line_end, last_line_end;
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; markdown_core_parser_input_ends answers true
     * for it once the input has run out. */
    const unsigned char *lookahead_cursor;
    /* THE ASKS: how many calls of the line have asked whether they meet a
     * candidate (markdown_core_parser_candidate_meets), the first numbered
     * 0, and how many had as the step being taken began. A reading holds a
     * line before a step, keeping the second in its stop, and a reading
     * that goes on from the stop starts from it, so a call has the same
     * number in every reading of its line (blocks.c, THE CANDIDATES). */
    size_t asks, step_asks;
} markdown_core_parser_at;

/* THE OLD TAIL OF AN EDIT (blocks.c, THE SPLICE): what the reopen took
 * from the flushed parse that the reading of the new text may take back
 * whole. The old root's children from the one the edit reads from on, in
 * order, which the records own (ast.c, THE RECORDS), and where the child
 * before them ended; the old lines from line `line` on; the reach and the
 * registry's records the reopen dropped; the definitions below the
 * children, each with the index of its child among them as its `start`,
 * in the order the old document listed them; the old reading's place at
 * its flush, and where its text was complete. The edit replaced the old
 * text up to `to` and moved what follows by `delta`. `next` is the child
 * the reading tries next and `point` where its line starts in the new
 * text, SIZE_MAX once none is left. Once the reading took the children
 * from `next` on back, `resumed` is where, and `tail` the index of the
 * first among the root's children, whose leads move by `lead`; 0 when it
 * took none back. */
typedef struct markdown_core_edit {
    struct markdown_core_node **children;
    size_t children_capacity;
    uint32_t count, next, tail, prior;
    /* THE BACK (docs/plans/relative-positions.md): the lines of the flushed
     * text past the input's, from its last to the first, each measured back
     * from `end`, the text's end: an edit before them moves none of them.
     * `line` is the first's number, in the text the reopen put back. */
    struct markdown_core_input_line *lines;
    size_t line, line_count, line_capacity, end;
    markdown_core_place *reach;
    size_t reach_count, reach_capacity;
    markdown_core_registry registry;
    markdown_core_defined *gone;
    size_t gone_count, gone_capacity;
    markdown_core_parser_at at;
    size_t complete, to, point, resumed;
    int64_t delta, lead;
} markdown_core_edit;

struct markdown_core_parser {
    /* The flags a block takes as it settles (blocks.c, A BLOCK SETTLES):
     * COMMITTED in a commit, nothing in a snapshot. */
    markdown_core_node_internal_flags settle_flags;
    /* Whether completion runs for a snapshot rather than for the finalize
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.4): a lookup sees only
     * the definitions that start before the block being completed, and one
     * that finds none is recorded (markdown_core_parser_find_definition). */
    bool snapshot;
    /* What the parse keeps as a stream (markdown_core_stream), made as the
     * parse starts. */
    markdown_core_stream *stream;
    /* What the parse of the text before a cursor reports of what it leaves
     * open (sink.h), or NULL: a parse with a sink is never published but
     * to the context query. */
    struct markdown_core_context_sink *sink;
    /* Every definition and explicit anchor the parse declared (registry.h):
     * the block phase declares definitions as it reads them, the headings
     * declare their labels before inline content is parsed, and completion
     * reserves explicit anchors where it meets them. */
    markdown_core_registry registry;
    /* The root being completed: the queued block or field root the
     * completion walk is in (blocks.c, COMPLETION), or the heading whose
     * inline content the document's preparation parses. */
    struct markdown_core_node *completing;
    markdown_core_source_order source_order;
    /* The stack the finish stage's tree walks borrow in turn, the finish
     * walk's frames and then publishing's: each grows it to what it needs
     * (markdown_core_parser_walk_stack) and leaves it to the next, and the
     * stage releases it when it ends. */
    void *walk_stack;
    size_t walk_stack_size;
    /* What a publish fills as it walks (ast.c, THE DEFINITION TABLES and AN
     * INLINE ROOT'S PLACES), which every publish of the parse reuses and the
     * stage releases with the walk stack. */
    void *definition_values[MARKDOWN_CORE_DEFINITION_TABLES];
    size_t definition_capacity[MARKDOWN_CORE_DEFINITION_TABLES];
    void *content_places;
    size_t content_place_capacity;
    /* THE COMPLETION QUEUE: every block of the tree and every field root a
     * block holds, in the order it settled (blocks.c, A BLOCK SETTLES), from
     * `completion_first` to `completion_last`, each linked through its
     * scaffold to the one after it and to the block it settled into or the
     * block that holds it (node.h, THE SCAFFOLD). A block joins once its
     * block structure is final, and a field root once the line that wrote it
     * is read; completing one parses its own inline content and asks the
     * finish steps at the events of its inline nodes and at its own EXIT,
     * with that block, after the block phase, in queue order
     * (S_finish_parse). Every one joins on its own, so a completion never
     * reaches another block or a block's field root. */
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
    /* WHERE THE READING STANDS (markdown_core_parser_at). */
    markdown_core_parser_at at;
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
    /* The storage the cells' inputs are read on, made as the first cell is
     * read and kept for the next while the document's input is read. */
    markdown_core_parser_input *cell_input;
    size_t input_line_work;
    /* A complete candidate may consume through a later source boundary. The
     * source driver advances to it after the current line has finished
     * (markdown_core_parser_claim). */
    const unsigned char *claimed_cursor;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* THE LINE'S VISIT: a number a reading of a line takes as it begins or
     * resumes the line (S_line_machine) or as a candidate's decision reads it
     * anew (markdown_core_parser_candidate_start), unless the last reading
     * read the same line of the same epoch, line `visit_line` of the input
     * of epoch `visit_epoch`, whose visit it goes on. The visit takes the
     * line's bytes in hand once (`curline`). A visit reads one line of one
     * text, so what an element reads of the line and of the lines after it
     * holds for the visit: every container the line opens asks again, at a
     * later byte, and a reading that resumes the line where a call asked to
     * be a candidate reads on from there. `epochs` counts the epochs the
     * parser has given. */
    size_t line_visit;
    size_t visit_epoch, epochs;
    int visit_line;
    /* THE LINE IN HAND: the bytes of the visit's line, through the line
     * feed a line machine reads it with. */
    markdown_core_strbuf curline;
    /* Whether a line machine or a candidate's decision reads the line in
     * hand: a block that closes while none does closes where the input's
     * last line ended. */
    bool reading;
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
    /* Whether the reading that asks has read the end of the input where
     * more text may follow since it cleared the record
     * (markdown_core_parser_see_end, markdown_core_parser_clear_end_read). */
    bool end_seen;
    /* The room of a lookahead (markdown_core_parser_lookahead_begin), one
     * block with `lookahead_capacity` places in each of its parts: the root
     * path an opener's lookahead builds, the builders from the active
     * input's root to the block's parent, and after it the flags of the
     * containers a lookahead matches, by their place in its path. Owned by
     * the parser so a document with many candidates allocates it once. */
    struct markdown_core_node **lookahead_path;
    struct markdown_core_lookahead_container *lookahead_containers;
    size_t lookahead_capacity;
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
    MARKDOWN_CORE_ARRAY(markdown_core_line_mark) line_marks;
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
    const markdown_core_line_mark *marks = parser->line_marks.values;
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
        const markdown_core_line_mark *mark = &parser->line_marks.values[span->first];
        span->start = mark->source + (offset - mark->content_offset) * mark->source_step;
        span->has_start = true;
        hint = span->first;
    }
    if (to >= 0) {
        bufsize_t offset = to + map->offset;
        span->last = markdown_core_block_content_mark_near(parser, map, offset, hint, &probes);
        hint = span->last;
        const markdown_core_line_mark *mark = &parser->line_marks.values[span->last];
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
    return parser->at.current;
}

/* Whether a block that closed has settled: every one has but the pending
 * block, which a later line can still write. */
/* WHAT THE COMMITS KEEP: in a snapshot, what `self`'s parse record keeps
 * with a mark (element.h, `kept_size`) as the earliest mark the reading can
 * be put back to found it -- the oldest candidate's, else the snapshot's --
 * and, in `*records`, the registry's length there. No put-back reaches what
 * the reading made before it. */
const void *markdown_core_parser_kept(const markdown_core_parser *parser, const markdown_core_element_instance *self,
                                      size_t *records);

static inline bool markdown_core_parser_settled(const markdown_core_parser *parser, const markdown_core_node *node) {
    return node != parser->at.pending;
}

/* A block an element installs in a field, which holds as a settled block
 * does from then on (blocks.c, A BLOCK SETTLES): the document's metadata,
 * which its prefix makes, and an inline note, which its Citation holds. */
static inline void markdown_core_parser_settle_field(const markdown_core_parser *parser, markdown_core_node *node) {
    node->flags |= parser->settle_flags;
}

/* The active input's root: the document, or the table cell whose content the
 * input is. */
static inline markdown_core_node *markdown_core_parser_block_root(const markdown_core_parser *parser) {
    return parser->at.block_root;
}

/* The builder `node`, an open block, is open inside: NULL for the active
 * input's root. */
static inline markdown_core_node *markdown_core_parser_builder_below(const markdown_core_parser *parser,
                                                                     const markdown_core_node *node) {
    return node == parser->at.block_root ? NULL : markdown_core_scaffold_parent(node);
}

/* The kinds of the blocks open on the builder stack, the current one down, and
 * of those that hold the active input's root, as block kind bits (node.h,
 * markdown_core_node_block_kind_bit). */
static inline uint32_t markdown_core_parser_open_kinds(const markdown_core_parser *parser) {
    uint32_t kinds = parser->input_enclosing;
    for (const markdown_core_node *open = parser->at.current; open;
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
    if (node == parser->at.pending) {
        return parser->at.current;
    }
    return markdown_core_parser_builder_below(parser, node);
}

/* The last child of `node`, an open block: the block open inside it, or,
 * when it is the current block, its pending child or else its last settled
 * one, each the last of its children (THE BUILDERS); NULL when it has
 * none. */
static inline markdown_core_node *markdown_core_parser_last_child(const markdown_core_node *node) {
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

/* The same of a block (dialect.h, markdown_core_dialect_block_kind). */
static inline const markdown_core_kind_record *markdown_core_parser_block_kind(const markdown_core_parser *parser,
                                                                               const markdown_core_node *block) {
    return markdown_core_dialect_block_kind(parser->dialect, (markdown_core_node_type)block->kind);
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
     * prefixes of the containers above it, as the block parser last matched
     * them, or the lookahead of the call that claimed the line
     * (markdown_core_parser_claim; markdown_core_parser_line_places). */
    uint32_t own;
} markdown_core_input_line;

typedef struct markdown_core_line_facts {
    struct markdown_core_normalized_line *normalized;
    /* Table grammar search facts under one matched container prefix. */
    const struct markdown_core_node *table_container;
    int table_offset;
    unsigned table_absent;
    /* A candidate matches open-container prefixes on every line it visits.
     * Failed candidates reaching the same line have nested container paths,
     * so each resumes from the deepest match already recorded. Each
     * (container, line) prefix is matched at most once per parse. NULL means
     * that no scan has recorded a prefix. */
    const struct markdown_core_node *container;
    /* Its place in the scan's path: path[depth] == container. */
    int depth;
    /* The line state after that container's prefix: what S_advance_offset left. */
    bufsize_t offset;
    bufsize_t column;
    bool partially_consumed_tab;
    /* Whether the line's bytes decided every prefix up to the container: on
     * a line the text has finished, and on one it has not, as a reading that
     * waits matched it; a reading that reads the end there decides a prefix
     * as the text ending would. */
    bool decided;
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
    return next < parser->input.lines.values + parser->input.lines.count ? next->start : parser->input.scanned;
}

/* Returned pointers are borrowed until the next request that grows the
 * index. Keep line numbers or copies across such a request. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index);

static inline markdown_core_input_line *markdown_core_parser_source_line(markdown_core_parser *parser, int line) {
    if (line < parser->input.first_line || parser->error) {
        return NULL;
    }
    size_t index = (size_t)(line - parser->input.first_line);
    return index < parser->input.lines.count ? &parser->input.lines.values[index]
                                             : markdown_core_parser_extend_source_lines(parser, index);
}

/* Whether the end of the active input is the end of its text (blocks.c, THE
 * CANDIDATES): false only in a session's commit of the document that more
 * text may follow. A snapshot, the last commit, a parse of a whole text and
 * a cell's content, which is complete when it is read, read their end. */
static inline bool markdown_core_parser_candidate_final(const markdown_core_parser *parser) {
    return parser->snapshot || parser->stream->ends || parser->at.block_root != parser->root;
}

/* THE END OF THE ACTIVE INPUT, learned in one place. Every reading that
 * asks whether the input goes on past a position asks here, or asks for a
 * line after the ones the driver reached (markdown_core_parser_source_line);
 * nothing else compares a position with the input's end. `cursor` is a
 * position of the active input. More text leaves every byte the input holds
 * as it is and moves only its end, so a reading that reaches the end reads
 * what more text can change. Where more text may follow
 * (markdown_core_parser_candidate_final), such a reading decides nothing:
 * the parser records that it read the end (`end_seen`), and the call it
 * reads for asks to be a candidate (blocks.c, THE CANDIDATES). A reading
 * that stops before the end decided from bytes alone, however far into the
 * last line it read. */
static inline void markdown_core_parser_see_end(markdown_core_parser *parser) {
    if (!markdown_core_parser_candidate_final(parser)) {
        parser->end_seen = true;
    }
}

/* A READING THAT ASKS WHETHER IT READ THE END (blocks.c, THE CANDIDATES):
 * an opener's call and a candidate's decision clear the record before they
 * read (`clear_end_read`) and ask it as they read and after
 * (`end_read`). Such readings do not nest. */
static inline void markdown_core_parser_clear_end_read(markdown_core_parser *parser) { parser->end_seen = false; }

static inline bool markdown_core_parser_end_read(const markdown_core_parser *parser) { return parser->end_seen; }

/* THE TEXT OF THE ACTIVE INPUT read so far, through `*length`: a reading of
 * it to its end asks markdown_core_parser_input_ends there. The document's
 * prefix reads it (element.h, `read_document_prefix`). */
static inline const unsigned char *markdown_core_parser_input_text(const markdown_core_parser *parser, size_t *length) {
    *length = parser->input.size;
    return parser->input.source;
}

static inline bool markdown_core_parser_input_ends(markdown_core_parser *parser, const unsigned char *cursor) {
    assert(cursor >= parser->input.source && cursor <= parser->input.source + parser->input.size);
    if (cursor < parser->input.source + parser->input.size) {
        return false;
    }
    markdown_core_parser_see_end(parser);
    return true;
}

/* WHERE THE LINES THE TEXT HAS FINISHED END (`complete`): a line that
 * starts at or past it is one the text has not finished, and asking here
 * reads nothing of the end. A reading that reads such a line to its end
 * reads the end (markdown_core_parser_see_end); a reading that can wait
 * waits on it instead, or reads it only as far as its bytes decide
 * (markdown_core_block_lookahead). The facts a lookahead keeps for a run of
 * blank lines hold finished lines only. */
static inline const unsigned char *markdown_core_parser_complete_end(const markdown_core_parser *parser) {
    return parser->input.source + parser->input.complete;
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
    if (line->facts) {
        return &parser->input.facts[line->facts - 1];
    }
    return markdown_core_parser_extend_line_facts(
        parser, markdown_core_array_write(&parser->input.lines, (size_t)(number - parser->input.first_line)));
}

/* The flags of a container a lookahead matches: as it first matched it, and
 * as the line the lookahead reads began, kept for a line it may stop before
 * (markdown_core_block_lookahead, `waited`). */
typedef struct markdown_core_lookahead_container {
    markdown_core_node_internal_flags flags, line_flags;
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
 * The containers it matches are its PATH, the builders from the active
 * input's root to the block's parent, root first, `depth` of them. `begin`
 * saves the parser's line state, the flags of each container are kept as a
 * line first matches it, from `touched` on, and `end` restores them; nothing
 * else in the parser or the tree is touched. `begin` returns false only when
 * an allocation failed, and has then marked the parse lost.
 *
 * A READING THAT CAN WAIT (blocks.c, THE CANDIDATES) stops at `limit`, which
 * `begin` leaves NULL for every other reading, and never reads the end of the
 * input (markdown_core_parser_see_end). Where a line would start at `limit`
 * it waits: `next` returns no line and sets `exhausted`, with the blank lines
 * it stepped over before the limit counted, and a reader that clears `limit`
 * reads on as an ordinary reading. `incomplete` says the line returned last
 * is one the text has not finished, and a reader that decides from that line
 * reads the end itself. With `waits`, such a line is read only as far as its
 * bytes decide each prefix (element.h, HOW FAR A STEP READS): where they do
 * not decide one, or leave the line blank so far, `next` returns no line and
 * sets `waited`, with the line unread, and a reader that clears `waits`
 * reads it on as an ordinary reading. */
typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_node *const *path;
    const unsigned char *cursor;
    const unsigned char *limit;
    /* The path's length, the first container a line has matched, and the
     * first the line being read matches. */
    int depth, touched, from;
    int line;
    int run_start;
    /* Where the reading stood as `begin` found it, which `end` puts back. */
    markdown_core_parser_at saved;
    bool active;
    bool exhausted;
    bool incomplete;
    bool waits;
    bool waited;
} markdown_core_block_lookahead;

/* A reading that can wait at the end of the input: an opener's call in a
 * commit more text may follow, and its decision as a candidate, read up to
 * the input's end and wait there, reading a line the text has not finished
 * only as far as its bytes decide (`limit` and `waits`). */
static inline void markdown_core_parser_lookahead_wait(markdown_core_block_lookahead *lookahead) {
    lookahead->limit = lookahead->parser->input.source + lookahead->parser->input.size;
    lookahead->waits = true;
}

/* The container the lookahead's block would join: the last of its path. */
static inline struct markdown_core_node *
markdown_core_parser_lookahead_parent(const markdown_core_block_lookahead *lookahead) {
    return lookahead->path[lookahead->depth - 1];
}

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
    assert(line >= parser->input.first_line && index < parser->input.lines.count);
    return &parser->input.lines.values[index];
}

/* The same, to change in place (array.h, markdown_core_array_write). */
static inline markdown_core_input_line *markdown_core_parser_rewritten_line(markdown_core_parser *parser, int line) {
    size_t index = (size_t)(line - parser->input.first_line);
    assert(line >= parser->input.first_line && index < parser->input.lines.count);
    return markdown_core_array_write(&parser->input.lines, index);
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
struct markdown_core_field;
/* THE OPEN CONSTRUCT a block scanner reads on the line being read, when the
 * parse reports to a context sink and the line is the document's last: it
 * opens a construct of kind `opens` whose opening syntax is the line's bytes
 * `from` up to `to`, and reads the `count` fields at `fields`, their offsets
 * on the line too, when the line ends. A parse with no sink lists nothing. */
void markdown_core_parser_open(markdown_core_parser *parser, markdown_core_node_kind opens, bufsize_t from,
                               bufsize_t to, const struct markdown_core_field *fields, size_t count);
/* The content of line `line` of the active input, which the driver read,
 * as the block parser reads it, a NUL as U+FFFD, without its terminator, and
 * its length; NULL, with the parse failed, when storage could not be had. */
const unsigned char *markdown_core_parser_line_content(markdown_core_parser *parser, int line, bufsize_t *length);
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
/* A CALL'S CLAIM on the lines after the reading's through `line`, which the
 * call's lookahead read: the reading skips them and goes on at `cursor`, the
 * last of them ending at `end`. Each claimed line's own bytes begin where
 * that lookahead matched the line's containers to, as the reading matches a
 * line it reads (markdown_core_input_line, `own`), so the call claims before
 * it places nodes on the lines. */
void markdown_core_parser_claim(markdown_core_parser *parser, int line, const unsigned char *cursor, bufsize_t end);
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

/* THE CANDIDATES (blocks.c). A call whose reading waited or read where the
 * text read so far ends, in a commit more text may follow, asks to be a
 * candidate in `container`, and declines: an opener's, the text step's, a
 * blank line's acceptance, or the document's prefix, which is called before
 * line 1 (element.h, `read_document_prefix`). `child` is the kind of block
 * it would open, whose lines its candidate is fed, `state` the record it
 * keeps of the grammar, the owner's `candidate_state_size` bytes, which the
 * request copies, and `ends` whether it could claim were the text to end after
 * the call's line (element.h, `feed_candidate`). The commit ends at the
 * call, completes, marks its state there unless an earlier call of the line
 * did, and reads on from the call. A call asks first whether it MEETS its
 * candidate, as the owner, in `container`, and asks for its candidate right
 * after: a call is the line and its number among the line's asks
 * (markdown_core_parser_at, THE ASKS). The
 * reading that made the candidate meets it as it reads on, and so does a
 * reading anew of its line after an accept, while it waits or once it is
 * rejected, and the call declines as it did. Once a later line decides, an
 * accept puts the commit back at the call, which reads the lines anew, and
 * a reject keeps what the commit read. */
void markdown_core_parser_request_candidate(markdown_core_parser *parser, const markdown_core_element_instance *owner,
                                            struct markdown_core_node *container, markdown_core_node_type child,
                                            const void *state, bool ends);
bool markdown_core_parser_meets_call(markdown_core_parser *parser, const markdown_core_element_instance *owner,
                                     struct markdown_core_node *container, size_t ask);
/* Every opener asks, so the parse reads whether a mark is left to meet
 * here and calls out only when one is. */
static inline bool markdown_core_parser_candidate_meets(markdown_core_parser *parser,
                                                        const markdown_core_element_instance *owner,
                                                        struct markdown_core_node *container) {
    const markdown_core_stream *stream = parser->stream;
    const size_t ask = parser->at.asks++;
    return stream->meet_mark < stream->mark_count && markdown_core_parser_meets_call(parser, owner, container, ask);
}
/* The decision of the candidate being decided (element.h, `decide_candidate`)
 * reads its line as its call did: `start` puts the parser's line state at
 * the call on a line, and gives the line through its line ending and the
 * container the call was asked in; `end` puts the line state back. False,
 * with the parse failed, when storage could not be had. */
bool markdown_core_parser_candidate_start(markdown_core_parser *parser, markdown_core_chunk *line,
                                          struct markdown_core_node **container);
void markdown_core_parser_candidate_end(markdown_core_parser *parser);
/* THE DECIDED CONTENT (blocks.c, THE CANDIDATES): what an accept can take
 * away or put back is UNDECIDED -- every block the first line's mark found
 * open or pending, its HOLDERS, the root among them, and everything that
 * settled after. A holder's children as the mark found them, the last
 * one aside when it is a holder too, are DECIDED, with everything they
 * hold, and so are the fields an accept keeps; so is every block while no
 * candidate waits. `holder` gives the entry of `block` at depth `depth`
 * (markdown_core_holder) when it is a holder, and false otherwise. */
bool markdown_core_parser_undecided(const markdown_core_parser *parser);
bool markdown_core_parser_holder(const markdown_core_parser *parser, size_t depth,
                                 const struct markdown_core_node *block, markdown_core_holder *entry);

/* THE DEFINITION LOOKUP of inline content: the record of `kind` that
 * declares `label`, as written, for the block being completed, or NULL.
 * Reference and footnote labels are looked up in their normal form, and a
 * specimen label as its bytes (registry.h). At a finalize every declaration
 * answers; at a snapshot only one whose key some definition declares before
 * the block starts, which is before its inline roots start, and a lookup that
 * finds none is recorded for the finalize. `*failed` is set when storage
 * could not be had, and the answer is then NULL. A snapshot's lookup goes on
 * from what the registry answered in markdown_core_parser_find_at_snapshot. */
const markdown_core_registry_record *markdown_core_parser_find_at_snapshot(markdown_core_parser *parser,
                                                                           markdown_core_registry_kind kind,
                                                                           const markdown_core_chunk *label,
                                                                           const markdown_core_registry_record *record,
                                                                           bool *failed);
static inline const markdown_core_registry_record *
markdown_core_parser_find_definition(markdown_core_parser *parser, markdown_core_registry_kind kind,
                                     const markdown_core_chunk *label, bool *failed) {
    const markdown_core_registry_record *record =
        kind == MARKDOWN_CORE_REGISTRY_SPECIMEN
            ? markdown_core_registry_find(&parser->registry, kind, label->data, label->len, failed)
            : markdown_core_registry_find_label(&parser->registry, kind, label, failed);
    return parser->snapshot && !*failed ? markdown_core_parser_find_at_snapshot(parser, kind, label, record, failed)
                                        : record;
}

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
 * `source` from where the transaction's last commit stopped: the text of
 * every commit of a transaction is the one before it with bytes added at its
 * end, and it is borrowed until the next commit or the end of the
 * transaction, and `ends` says it is the whole text, which no commit
 * follows. A commit of a text more text may follow reads the lines the text has
 * finished and the steps of its last line that the line's bytes decide
 * (blocks.c, THE LINE A SESSION HOLDS). False when the transaction failed.
 * `finalize` ends the input there:
 * every block still open closes, the closed blocks complete, and the tree is
 * published and returned, or NULL when the transaction failed. `discard`
 * ends a transaction and returns nothing. Both release the transaction's
 * state, so an instance runs any number of them, each as the first.
 * `flush` is `finalize` without the release: the state stays, for a
 * session's edits, until a `discard` releases it. */
void markdown_core_parser_start(markdown_core_parser *parser, markdown_core_revision *revision);
bool markdown_core_parser_commit(markdown_core_parser *parser, const char *source, size_t length, bool ends);
markdown_core_node *markdown_core_parser_finalize(markdown_core_parser *parser);
markdown_core_node *markdown_core_parser_flush(markdown_core_parser *parser);
void markdown_core_parser_discard(markdown_core_parser *parser);
/* A flushed transaction put back at input line `line`, a rest point of
 * the flushed document `root` before its child `child`, to read the new
 * text `source` of an edit that replaced the old text from `from` to `to`
 * and moved what follows by `delta`: a commit that ends the text and a `flush` read
 * it from there (blocks.c, THE REOPEN), and take the old children after the
 * edit back where they read as they did (THE SPLICE), unless `whole` says
 * to read all of it. False, with the transaction failed, when storage could
 * not be had. */
bool markdown_core_parser_reopen(markdown_core_parser *parser, markdown_core_node *root, uint32_t child, size_t line,
                                 size_t from, size_t to, int64_t delta, const char *source, bool whole);
/* Where the last reading of an edit took the old children back, in the new
 * text: 0 when it took none back. */
size_t markdown_core_parser_resumed(const markdown_core_parser *parser);
/* The root's child the old children it took back begin at, when it took
 * some back. */
uint32_t markdown_core_parser_tail(const markdown_core_parser *parser);
/* Where the text an edit's back is measured from ends (THE BACK). */
static inline uint32_t markdown_core_parser_back_end(const markdown_core_parser *parser) {
    return (uint32_t)parser->stream->edit->end;
}
/* THE LINES OF A FLUSHED TRANSACTION'S TEXT, counted from 0: the line that
 * holds `offset`, and line `line`'s start and the start of the line after
 * it, or the text's end -- false when the text has no such line. A text
 * that ends with a line ending, or is empty, has an empty last line at its
 * end. */
size_t markdown_core_parser_line_of(const markdown_core_parser *parser, size_t offset);
/* The parse just started takes edits after its flush (markdown_core_stream,
 * `edit`). False, with nothing changed, when storage could not be had. */
bool markdown_core_parser_edits(markdown_core_parser *parser);
/* Whether it does. */
bool markdown_core_parser_takes_edits(const markdown_core_parser *parser);
/* A reading of the document decided what starts at `start` from the bytes up
 * to `end` (THE REACH). The parse fails when storage could not be had. A
 * parse that takes no edits pays the test and nothing more, and so does a
 * snapshot, whose readings its rollback takes back. */
void markdown_core_parser_note_reach(markdown_core_parser *parser, size_t start, size_t end);
static inline bool markdown_core_parser_reaches(const markdown_core_parser *parser) {
    return parser->stream && parser->stream->edit && !parser->stream->snapshot_mark;
}
static inline void markdown_core_parser_reach(markdown_core_parser *parser, size_t start, size_t end) {
    if (markdown_core_parser_reaches(parser) && end > start) {
        markdown_core_parser_note_reach(parser, start, end);
    }
}
/* Puts the reach of the flushed document in the order its queries need;
 * it reads as it did. */
void markdown_core_parser_sort_reach(markdown_core_parser *parser);
/* The first start of a reading of the flushed document that spans `point`,
 * reading past it from before it, in `*start`: false when none does. The
 * reach is sorted (markdown_core_parser_sort_reach). */
bool markdown_core_parser_spanned(const markdown_core_parser *parser, size_t point, size_t *start);
bool markdown_core_parser_line_span(const markdown_core_parser *parser, size_t line, size_t *start, size_t *end);
/* Completes the blocks that have settled, for a snapshot (plan 5.4): the
 * cells of the tables that closed are read, the document prepares the
 * headings that settled, and the completion queue is completed with a
 * snapshot's lookups. What it completes stays complete. False when the
 * transaction failed. */
bool markdown_core_parser_complete(markdown_core_parser *parser);
/* A SNAPSHOT of `source`, the text of the last commit (blocks.c, THE
 * SNAPSHOT): the committed parse read on to the end of the text, ended
 * there, completed with a snapshot's lookups and published, and then put
 * back as its commit left it. The published root, which the next commit,
 * finalize or discard takes back, or NULL when the transaction failed. */
markdown_core_node *markdown_core_parser_snapshot(markdown_core_parser *parser, const char *source, size_t length);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
