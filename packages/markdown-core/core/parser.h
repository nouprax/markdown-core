#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include "measure.h"
#include <assert.h>
#include <stdint.h>
#include "array.h"
#include "tree.h"
#include "labels.h"
#include "names.h"
#include "node.h"
#include "buffer.h"
#include "dialect.h"
#include "reader.h"

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
 * A session's parse publishes the tree it builds, in place, at its finish
 * and at each edit's, and each publish continues the one before
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
 * the runs `markdown_core_text_share(from, to - from + 1)` then searched for again.
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

/* AN INPUT the block parser reads: its bytes, and the geometry of its
 * lines. The driver and lookahead extend one index; a source
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
    /* The lines, in a tree (tree.h) whose back an edit keeps: the lines of
     * the flushed text past the input's, in the old text's terms until the
     * reading takes them back (THE SPLICE, blocks.c). The front is the
     * input's own, which the parse reads and grows. */
    markdown_core_tree lines;
    /* Where the text ends that the back of the other trees is in the terms
     * of (THE BACK). */
    size_t back_end;
    /* THE SCAN MEMO of the reading (blocks.c, THE LOOKAHEAD): what the
     * lookaheads of one reading of the input learned of the lines they
     * visited, line `memo_base` first, so a later lookahead of the same
     * reading resumes where an earlier one matched to. It is the reading's
     * alone: each reading starts it empty (S_parse_source). */
    struct markdown_core_line_memo *memo;
    int memo_base;
    size_t memo_count, memo_room;
    /* Whether a column of the input may stand elsewhere than the same
     * offset from its line's start in the source: true for a cell's
     * content. */
    bool mapped;
    /* Whether the scan found a line that ends with `\r`, which a block of
     * literal lines writes as `\n` (blocks.c, THE LEAP): once it does, for
     * the rest of the document's life. */
    bool returns;
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

/* A RECORD A PUBLISH KEPT (ast.c, THE RECORDS): a node of a relation of a
 * node the publish walked in place that was not FINAL: its id and kind,
 * where its published source lay, the owner's relation it was in, the next
 * record of the same owner, plus one, or 0, and the first record the publish
 * kept of the node's own relations, plus one, or 0. An old node an edit's
 * reading left out of the tree, which the next publish may match, is the
 * record's `node`, which the record owns (ast.c, markdown_core_publish_cede);
 * a node that stays in the tree is no record's node. A free record is
 * linked through `next`. */
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
/* The `count` top-level nodes `nodes` the stream settled in the content of
 * the paragraph `root` past its FINAL bound leave it, as the line they lie
 * on leaves it (blocks.c, A SETTLED LINE THAT LEAVES): the record the last
 * publish kept of each one, which stayed in the tree, owns it from now on
 * (ast.c, THE RECORDS), and the next publish matches the new content
 * against them. One no record names is released. */
void markdown_core_publish_cede(markdown_core_parser *parser, const markdown_core_node *root,
                                markdown_core_node *const *nodes, uint32_t count);

/* The places of the published node `node` whose source starts at `start`,
 * read from its runs (elements/ast.c, THE COORDINATES OF A NODE), in
 * storage of `pool`; NULL when it could not be had. */
markdown_core_places *markdown_core_node_places_of(markdown_core_node_pool *pool, const markdown_core_node *node,
                                                   uint32_t start);
/* The same of the `count` nodes `nodes` the content of `root` lets go, of
 * its relation `holder` holds -- its children's when that is `root`, or a
 * field root's -- the first of which the last publish measured from
 * `source` bytes past where the root starts (blocks.c, THE SEAMS). */
bool markdown_core_publish_keep_left(markdown_core_parser *parser, const markdown_core_node *root,
                                     const markdown_core_node *holder, markdown_core_node *const *nodes, uint32_t count,
                                     uint32_t source);

/* A DOCUMENT'S TABLES (ast.c): the definitions the published document
 * holds -- Footnotes, Specimens, References, Headings -- are the labels'
 * definitions of their nodes, a sequence of each kind (labels.h, THE
 * SEQUENCES), which the document reads as the labels keep them, and its
 * reference labels, which the labels keep in order (labels.h, THE NAMES
 * IN ORDER). */
enum { MARKDOWN_CORE_DEFINITION_TABLES = 4 };

typedef struct markdown_core_tables {
    /* The labels the tables read: the parse's, or `held`, once a document
     * took them from a parse that takes no edits. */
    markdown_core_labels *labels;
    markdown_core_labels held;
    /* The space of each table's labels. */
    unsigned kinds[MARKDOWN_CORE_DEFINITION_TABLES];
} markdown_core_tables;

/* The tables' storage let go, and the labels they hold. */
void markdown_core_publish_release_tables(markdown_core_tables *tables);

/* WHERE AN EDIT'S READING STANDS INSIDE A LINE (blocks.c, A REST INSIDE A
 * LINE): the line has matched every open block and opened `container`,
 * the block that takes the rest of the line as text; the line's cursor
 * there, how far the steps before it read (reader.h, `far`), where its own
 * bytes begin, and whether the line's first reading looked past its end
 * (`past`). */
typedef struct markdown_core_line_stop {
    struct markdown_core_node *container;
    bufsize_t offset, column, far;
    uint32_t own;
    bool past;
} markdown_core_line_stop;

/* THE SETTLE POINT OF AN INLINE ROOT (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.8). A root an edit's reading reads from a seam is parsed from `from`,
 * a token start before which its nodes are its caller's (0: the whole
 * content). The parse lists
 * in `starts` the starts of the top-level tokens it read before any token
 * read the end of the content, where nothing read before holds anything
 * open, and leaves in `undecided` the earliest start of
 * a token whose reading read the end, or of an opener left unmatched,
 * INT32_MAX when there is none. It lists in `notes` the inline notes it
 * made, fields their Citations hold, which commit with the nodes that hold
 * them as those settle. `source` is where `from` was written, measured
 * from where the root's source starts, as its published fields are. */
typedef struct markdown_core_inline_settle {
    bufsize_t from, undecided;
    uint32_t source;
    bufsize_t *starts;
    size_t count, capacity;
    struct markdown_core_node **notes;
    size_t note_count, note_capacity;
} markdown_core_inline_settle;

/* WHAT A PARSE KEEPS of its reading, which publishes its tree in place.
 * The parser owns it from markdown_core_parser_start until the parse is
 * released. */
typedef struct markdown_core_stream {
    /* The line an edit's reading stands inside, when `held`. */
    markdown_core_line_stop stop;
    bool held;
    /* THE RECORDS (ast.c). */
    markdown_core_tail_records records;
    /* The definitions the document holds (ast.c, A DOCUMENT'S TABLES):
     * made at the first publish (ast.c, tables_need); a parse that takes
     * no edits gives them to its document. */
    markdown_core_tables *tables;
    /* THE EDIT the next publish follows (blocks.c, THE REOPEN): it replaced
     * the old text from `edit_from` to `edit_to` and moved what follows by
     * `edit_delta`; all zero for every other publish (ast.c, THE IMAGE OF AN
     * OLD ANCHOR). */
    uint32_t edit_from, edit_to;
    int64_t edit_delta;
    /* What the edits that read the parse again after its flush keep: a
     * session's (markdown_core_parser_edits), which keeps the cuts, the
     * headings' state and the old tail of each edit (THE SPLICE); NULL for
     * a one-shot parse, which keeps none of them. */
    struct markdown_core_edit *edit;
    /* The paragraph an edit's reading reopened at a seam, whose completion
     * parses its content from there (blocks.c, THE SEAMS), or NULL. */
    struct markdown_core_node *reopened;
} markdown_core_stream;

/* WHERE A READING STANDS: every field of the parser a reading of lines
 * writes as it goes, one value, which what puts a reading back -- a
 * lookahead
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
     * document labels. Input roots stay owned by the AST; the queue only
     * borrows them until their block content is read. */
    struct markdown_core_node *block_root, *current, *pending;
    /* The line being read (reader.h, THE READER): the driver records its
     * number, start and end as it hands the line to the block parser, they
     * hold while `curline` does, and a claim of later lines moves the start.
     * Where a thematic break failed on it, and where the last line read
     * ended. */
    markdown_core_reader read;
    bufsize_t thematic_break_kill_pos, last_line_end;
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; markdown_core_parser_input_ends answers true
     * for it once the input has run out. */
    const unsigned char *lookahead_cursor;
} markdown_core_parser_at;

/* THE OLD TAIL OF AN EDIT (blocks.c, THE SPLICE): what the reopen took
 * from the flushed parse that the reading of the new text may take back
 * whole. The old root's children from the one the edit reads from on, in
 * order, which the records own (ast.c, THE RECORDS), and where the child
 * before them ended; the old lines from line `line` on; the definitions
 * below the
 * children, each with the index of its child among them as its `start`,
 * in the order the old document listed them; the old reading's place at
 * its flush, and where its text was complete. The edit replaced the old
 * text up to `to` and moved what follows by `delta`. `next` is the child
 * the reading tries next and `point` where its line starts in the new
 * text, SIZE_MAX once none is left. Once the reading took the children
 * from `next` on back, `resumed` is where, and `tail` the index of the
 * first among the root's children, whose leads move by `lead`; 0 when it
 * took none back. */
struct markdown_core_folds;

/* WHERE A USE IS (blocks.c, S_use_place): the place of the content's
 * places `places` of the root `root` the last use was in, and the content
 * offset it starts at. */
typedef struct markdown_core_use_walk {
    const struct markdown_core_node *root;
    const struct markdown_core_places *places;
    uint32_t place, read;
} markdown_core_use_walk;

/* Where a block of an edit's chain (markdown_core_edit, `chain`) starts in
 * the old text, and where the last child of its FINAL prefix ends there,
 * or where it starts when it has none (ast.c, THE CHAIN'S RECORDS). */
typedef struct markdown_core_chain_place {
    uint32_t start, prior;
} markdown_core_chain_place;

/* AN OPENER READ TO THE END (blocks.c): `owner`'s, on the line that
 * starts at `start`; `aside` while an edit's reading reads the text again
 * from before it (THE SPLICE). */
typedef struct markdown_core_unclosed {
    const struct markdown_core_element_instance *owner;
    uint32_t start;
    bool aside;
} markdown_core_unclosed;

/* THE LEAP (blocks.c): an edit whose rest point is a later line of a block
 * of literal lines keeps the block's old content from that line on in the
 * back of `aside` (`cr` when it holds a carriage return; THE BACK OF A
 * PARAGRAPH), and the reading leaps
 * over the old lines past the edit while the block stays open, from the
 * line that starts at `at` of the new text, SIZE_MAX when it does not, to
 * the back's line `last` from the focus, the last of the block's whose
 * content it reads again. `next` is where the old child the reading tries
 * next starts its line (THE SPLICE). `blanks` when a block of the chain
 * above it keeps flags that a line sets (element.h, the lines' `speculative`).
 * `prose` when the block was a paragraph as the reading reopened it, whose
 * inline content it reads again whatever it becomes.
 *
 * THE SEAMS of that paragraph's inline content (blocks.c): the paragraph
 * `node`, whose id is `id`, as the last edit's reading completed it, and
 * the `seam_count` seams it found then, in order; `seam` the one the
 * reading takes its first children to, all zero for none. The
 * reading's parse of the content lists what may be seams in `settle`,
 * from the child `held` on, while `active`. */
typedef struct {
    /* Where it is in the content, the first child after it, and the line of the paragraph
     * it starts, counted from its first; where the child before it ends in
     * the text, past where the paragraph starts, as the content's places
     * read it; and the place of the content's places that reads its last
     * byte, and the content offset that place starts at. */
    bufsize_t offset;
    uint32_t index, line, source, place, read;
    /* How far before it the child before it ends in the content. */
    uint32_t lead;
    /* The measure the element of the paragraph's kind takes of the children
     * before it (element.h, the finish's `measure`), zero for none; a seam
     * the back holds measures those after it. */
    markdown_core_measure measure;
    /* Whether it lies inside the content's last line, `base` bytes past
     * where that line starts: a seam only an edit at the content's end
     * keeps (blocks.c, THE SEAMS). */
    bool inner;
    uint32_t base;
} markdown_core_seam;

/* THE SEAMS a paragraph's parse may meet (blocks.c): the old seams past
 * the edit, `count` of them, the nearest last, each measured back from the
 * content's `end`; `met` once the parse stopped at the nearest. */
typedef struct markdown_core_seam_stops {
    const markdown_core_seam *items;
    size_t count;
    bufsize_t end;
    bool met;
} markdown_core_seam_stops;

typedef struct markdown_core_leap {
    markdown_core_split *aside;
    size_t at, next, last;
    bool blanks, prose;
    /* THE RUNS OF A SPLICE (blocks.c): the input line the block's reading
     * began at, `top`; the first of the block's runs that does not end
     * before it, `low`, where it starts, `low_start`, and the bytes of own
     * source the runs before it hold, `low_own`; and the pin the last
     * splice in a block left there, read by the next edit's reading, which
     * finds those runs from it: the edit's `round`, the block, its runs and
     * their count, the run, where it starts from the block's start, and the
     * bytes before it. */
    size_t top, low_start, low_own, round, pin_round, pin_start, pin_own;
    uint32_t low, pin_index, pin_count;
    const struct markdown_core_node *pin_node;
    const struct markdown_core_runs *pin_list;
    bool armed, active;
    const struct markdown_core_node *node;
    uint64_t id;
    markdown_core_seam *seams;
    size_t seam_count, seam_room;
    markdown_core_seam seam;
    uint32_t held;
    markdown_core_inline_settle settle;
    /* THE OLD TAIL OF A PARAGRAPH (blocks.c): the seams past the edit, as
     * stops measured back from where the paragraph's content, children,
     * lines, source and places ended (`ends`), the nearest last; the
     * paragraph's old children past the seam, still in its storage, from
     * `seam.index` up to `count`, while `kept`; where the leap landed, `land` bytes
     * before the old content's end, and where the new content had come to
     * then, `landed`, while `lands`; once the parse met a stop, the child
     * the old tail starts at, `tail`, and where the reading's children end
     * among the paragraph's, `spliced`, which the publish walks to. */
    markdown_core_seam *backs;
    size_t back_count, back_room;
    markdown_core_seam ends;
    uint32_t count, tail, spliced;
    size_t land, landed;
    bool kept, lands;
    markdown_core_seam_stops stops;
    /* What the paragraph's inline runs gave up from the seam on, as the
     * last publish measured it (node.h, OPENER), set aside while the parse
     * reads past the seam: the old tail it meets keeps those in it. */
    struct markdown_core_written *opens;
    uint32_t open_count;
    size_t open_room;
    /* THE PREFIX OF A PARAGRAPH (blocks.c): the content marks the last
     * reading of `node` read its content with, `marked` of them, which
     * stand right after the identity run until the next reopen or look,
     * their lines counted from `base`; and the places that read made, a
     * copy, `places` NULL for none. The marks of the old content from the
     * rest point's line on, which starts at content offset `cut` of the old
     * content, wait in `marks`, `mark_count` of them, for the leap to take
     * those of the lines it lands over back: the last `mark_count` of the
     * `mark_room` at `marks`, each moved by `shift` (its content offset,
     * line and source). `final` is the last of the marks as the reading
     * left it, which the reopen finds them by. A paragraph that took the
     * rest of the text back (THE SPLICE IN A PARAGRAPH) keeps the marks of
     * its content's back there, `apart`, past the `apart_count` its map
     * holds from `apart_first` (THE BACK OF A PARAGRAPH). */
    markdown_core_line_mark final;
    uint32_t marked;
    int base;
    markdown_core_places *places;
    markdown_core_line_mark *marks;
    size_t mark_count, mark_room;
    markdown_core_line_mark shift;
    bufsize_t cut, apart_first, apart_count;
    bool apart;
    /* THE PANE (blocks.c): a paragraph in a block that holds blocks, whose
     * runs the splice wrote (THE RUNS OF A SPLICE), keeps them, `owed` its
     * line places while it does; and its parse from a seam reads its content
     * with places only of a pane of it, `pane`, while `paned`: from content
     * offset `pane_from`, after one place that stands for the content
     * before, up to `pane_to`, of the `pane_size` bytes the parse reads,
     * the content marks `map` held before the read, from the mark
     * `pane_mark` of them on, the last place ending at `pane_at`, its own
     * source from the run `pane_run`, which starts at `pane_start`, on;
     * `pane_end` once it holds the content's end and the runs after it. */
    bool owed, paned, pane_end;
    /* THE MEASURE of the paragraph's children (blocks.c): its `ends`'
     * measure, once its last parse from a seam measured them, `measured`;
     * the seams from `fresh` on the parse listed are to be measured while
     * `owing`, the seam of the back it met measuring `stop`; the seams'
     * measures stand from the first child on, `metered`, and did as the
     * parse began, `based`. */
    bool measured, owing, metered, based;
    size_t fresh;
    markdown_core_measure stop;
    markdown_core_content_map map;
    markdown_core_places *pane;
    size_t pane_room, pane_mark, pane_start;
    uint32_t pane_from, pane_to, pane_size, pane_at, pane_run;
} markdown_core_leap;

/* THE OLD TAIL IN PLACE (blocks.c, seam.h): a holder's old tail, `held`
 * children, stays in the holder's own storage from its child `start` on --
 * the first the reading writes -- and the reading writes its children over
 * it from there: the first `saved` of the old ones, each moved out as the
 * reading took its place, are in the edit's room for them from its entry
 * `base` on, and the rest still where they were. */
typedef struct markdown_core_seam_tail {
    uint32_t start, held, saved, base;
} markdown_core_seam_tail;

/* A block a reading gave the kind it read its lines as (blocks.c, THE
 * FORMER KIND): the block, NULL for none, and its kind and id before, as
 * the node the publish named, which it continues once its close or
 * completion makes it of that kind again. */
typedef struct markdown_core_revert {
    struct markdown_core_node *node;
    uint64_t id;
    uint16_t kind;
    /* What the element's `revert` kept for its `redo` (element.h). */
    uint32_t kept;
} markdown_core_revert;

typedef struct markdown_core_edit {
    /* The root's old tail (THE OLD TAIL IN PLACE), its `start` the root's
     * child the reading began at, and its room `children`, which has room
     * for all of it (markdown_core_edit_old). */
    markdown_core_seam_tail root;
    struct markdown_core_node **children;
    size_t children_capacity;
    /* Where the child before the old tail ends, and where the child before
     * `next` does, in the old text: the old children are measured from
     * there by their extents, as nothing moves their places (THE SPLICE). */
    uint32_t count, next, tail, prior, before;
    /* THE NESTED REOPEN (blocks.c): the `depth` blocks below the root the
     * reading reopened, the root's child first, each the last child of the
     * one before; `path` holds where each is among its holder's children,
     * and past them where the deepest's first child that goes was. */
    struct markdown_core_node **chain;
    uint32_t *path;
    markdown_core_chain_place *places;
    size_t depth, chain_capacity;
    /* THE NESTED SPLICE (blocks.c): for each block of the chain, where it
     * ended in the old text (`ends`), its flags there (`states`), its
     * record in the publish's (`owners`, ast.c), where the children the
     * reading took back begin among its own (`tails`), UINT32_MAX when it
     * took none, its old tail (`levels`), whose `start` is how many
     * children it keeps, and the fold marks
     * of its children (`folds`), or NULL. `shift` is what the lead of the
     * first it took back moves by. `closes` is where the old close of
     * each ended, 0 when its block's close did not read past the rest
     * point (node.h, `closed`).
     *
     * THE OLD NESTED TAIL IN PLACE (blocks.c): each block of the chain
     * holds its old tail in its own storage as the root does (THE OLD TAIL
     * IN PLACE), its room `nested`, which has room for all of them,
     * `nest_count`, numbered in
     * the order of the text, the deepest block's first. The reading tries
     * them in that order: the next is the old child `nest_index` of the
     * block `nest_level`, the one before it ended at `nest_prior` in the old
     * text, and `nest_on` is false once it tried them all. `taken` is the
     * number of the first it took back, SIZE_MAX when it took none, and
     * `level` its block. The first `nest_recorded` have records (ast.c,
     * record_nest); those past them up to the first taken back the parse
     * lets go when it fails. */
    uint32_t *ends, *states, *owners, *tails, *closes, *bases;
    markdown_core_seam_tail *levels;
    struct markdown_core_folds **folds;
    struct markdown_core_node **nested;
    size_t nested_capacity, taken, level, nest_level;
    uint32_t nest_count, nest_index, nest_prior, nest_recorded;
    uint32_t opener_count;
    int64_t shift;
    bool nest_on;
    /* The chain's deepest block when the reopen gave it the kind it read
     * its lines as (blocks.c, THE FORMER KIND), NULL otherwise. */
    markdown_core_revert revert;
    /* Whether the parse is an edit's reading. */
    bool reading;
    /* The number of the back's first line (parser_input, `lines`), in the
     * text the reopen put back. */
    size_t line;
    /* AN OPENER READ TO THE END (blocks.c): the openers whose readings note
     * no reach, `opener_count` of them, in storage of just that size, as
     * they are few and seldom change. */
    markdown_core_unclosed *openers;
    /* THE LEAP (blocks.c), made at the first edit that rests in a block of
     * literal lines. */
    markdown_core_leap *leap;
    /* What the line starts of the lines the deepest block reads again
     * gave up (node.h, OPENER), set aside at the reopen as raw entries of
     * the old text: those of old lines the reading takes back unread (THE
     * LEAP, THE SPLICE IN A PARAGRAPH) come back, moved by the edit. */
    struct markdown_core_written *line_opens;
    uint32_t line_open_count;
    size_t line_open_room;
    /* The old tail's children no record holds yet, nor the root, the
     * `unrecorded` from `recorded` (ast.c, record_tail): the parse lets
     * them go when it fails before its publish. */
    uint32_t recorded, unrecorded;
    markdown_core_parser_at at;
    /* Where the line ends, past its line ending, that the splice closes a
     * block of the chain on (blocks.c, S_close_inside), as its old reading
     * did; 0 when it closes none. */
    size_t shut;
    /* A look read on the line in hand by a reading that names no block, its
     * start and end: the line's block takes it as the line ends
     * (markdown_core_parser_reach), and that line. End 0 for none. */
    markdown_core_place look;
    int look_line;
    /* A lookahead that leaves the reading as it was reads in hand: what it
     * reads is noted nowhere, in a node nor in a line. */
    bool quiet;
    /* The furthest any reading the reading of the edit noted since its
     * reopen read (THE REACH), 0 for none; and the child index of the text
     * before the edit (index.h), which the old tail's readings are read
     * from, and where the children of each block of the chain are measured
     * from in it (`bases`), and the root's (`base`). */
    size_t ran;
    /* The cuts of looks whose last byte lies past the lines read in, each
     * set on its line as the line is read in (blocks.c, THE CUTS). */
    MARKDOWN_CORE_ARRAY(markdown_core_place) cuts;
    /* The cuts before `cut_low` are set; one that starts where it ends
     * keeps the cut its line had before the reopen let it go. */
    size_t cut_low;
    const struct markdown_core_index *index;
    uint32_t base;
    size_t complete, to, point, resumed;
    int64_t delta, lead;
} markdown_core_edit;

/* The old tail's child `index`, of the root `root` (THE OLD TAIL IN
 * PLACE). */
static inline struct markdown_core_node *markdown_core_edit_old(const markdown_core_edit *edit,
                                                                const struct markdown_core_node *root, uint32_t index) {
    return index < edit->root.saved ? edit->children[index] : root->children[edit->root.start + index];
}

/* The old child `index` past those it keeps of the chain's block `level`
 * (THE OLD NESTED TAIL IN PLACE). */
static inline struct markdown_core_node *markdown_core_edit_nested(const markdown_core_edit *edit, size_t level,
                                                                   uint32_t index) {
    return index < edit->levels[level].saved ? edit->nested[edit->levels[level].base + index]
                                             : edit->chain[level]->children[edit->levels[level].start + index];
}

struct markdown_core_parser {
    /* What the parse keeps as a stream (markdown_core_stream), made as the
     * parse starts. */
    markdown_core_stream *stream;
    /* Every definition, explicit anchor and reference the parse made
     * (labels.h):
     * the block phase declares definitions as it reads them, the headings
     * declare their labels before inline content is parsed, and completion
     * reserves explicit anchors where it meets them. */
    markdown_core_labels labels;
    /* What the block starts of the line in hand began and gave up
     * (markdown_core_field_opener with no node), placed in the source, which the
     * block that takes the line's text keeps (node.h, OPENER). */
    struct markdown_core_written *line_opens;
    uint32_t line_open_count;
    size_t line_open_capacity;
    /* The nodes that ask for names and the names they take (names.h). */
    markdown_core_names names;
    /* The root being completed: the queued block or field root the
     * completion walk is in (blocks.c, COMPLETION), or the heading whose
     * inline content the document's preparation parses. */
    struct markdown_core_node *completing;
    markdown_core_use_walk use;
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
    /* The passes of finished readings (inlines.c, markdown_core_pass),
     * kept for the next, a reading inside a reading taking its own. */
    struct markdown_core_pass *free_passes;
    /* The sealed dialect this instance parses with (dialect.h), which
     * shares the instance's allocation (blocks.c, markdown_core_instance),
     * and the context its setup was given. The context is the caller's; the
     * engine only carries it to element hooks. */
    const markdown_core_dialect *dialect;
    void *context;
    /* The root node of the parser, of the dialect's root kind */
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
    /* The child count at which the next child of `room_holder`, the root or
     * a block of an edit's chain, takes the place of an old child of an
     * edit's tail, which moves out first (blocks.c, THE OLD TAIL IN PLACE);
     * UINT32_MAX when none is in place. `room_level` is the chain's block
     * that holds it, and `root_room` the root's, while a block of the chain
     * does. */
    uint32_t room_at, root_room;
    const struct markdown_core_node *room_holder;
    size_t room_level;
    /* The active input: the document's text, or a table cell's content
     * while its blocks are read. */
    markdown_core_parser_input input;
    /* The storage the cells' inputs are read on, made as the first cell is
     * read and kept for the next while the document's input is read. */
    markdown_core_parser_input *cell_input;
    size_t input_line_work;
    /* A complete block start may consume through a later source boundary. The
     * source driver advances to it after the current line has finished
     * (markdown_core_parser_claim). */
    const unsigned char *claimed_cursor;
    int claimed_line;
    bufsize_t claimed_last_end;
    /* THE LINE'S VISIT: a number a reading of a line takes as it begins or
     * resumes the line (S_line_machine), unless the last reading
     * read the same line of the same epoch, line `visit_line` of the input
     * of epoch `visit_epoch`, whose visit it goes on. The visit takes the
     * line's bytes in hand once (`curline`). A visit reads one line of one
     * text, so what an element reads of the line and of the lines after it
     * holds for the visit: every container the line opens asks again, at a
     * later byte. `epochs` counts the epochs the
     * parser has given. */
    size_t line_visit;
    size_t visit_epoch, epochs;
    int visit_line;
    /* THE LINE IN HAND: the bytes of the visit's line, through the line
     * feed a line machine reads it with. */
    markdown_core_strbuf curline;
    /* Whether a line machine reads the line in hand: a block that closes while none does closes where the input's
     * last line ended. */
    bool reading;
    /* Whether a paragraph's marks were ever kept apart in an edit's leap
     * (blocks.c, THE BACK OF A PARAGRAPH): only then may a placement read
     * a mark from the back. */
    bool apart;
    /* Options set by the user, see the Options section in markdown_core.h */
    /* Sticky allocation-failure flag: once any parse structure is lost, the
     * one-shot transaction reports the whole parse as failed (NULL) instead of
     * returning a silently truncated document. */
    markdown_core_parse_error error;
    /* THE ENGINE'S WORK COUNTERS, for deterministic complexity gates. An
     * element counts its own work in its own state record. */
    /* The bytes the inline tokenizer read again after an attempt or a
     * check gave them back (tokenizer.h, `rewound`). */
    size_t rewound_work;
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
    /* The room of a lookahead (markdown_core_parser_lookahead_begin), one
     * block with `lookahead_capacity` places in each of its parts: the root
     * path an opener's lookahead builds, the builders from the active
     * input's root to the block's parent, and after it the flags of the
     * containers a lookahead matches, by their place in its path. Owned by
     * the parser so a document with many lookaheads allocates it once. */
    struct markdown_core_node **lookahead_path;
    struct markdown_core_lookahead_container *lookahead_containers;
    size_t lookahead_capacity;
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

/* The engine's places of a text (markdown-core-element-api.h, THE PLACES
 * OF A TEXT): the source line, counted from 1, and the source offset where
 * byte `offset` of the text `map` places begins, or, `end_place`, ends; 0,
 * leaving both outputs untouched, when the map places no byte. */
int markdown_core_parser_content_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                       bufsize_t offset, int *line, bufsize_t *source);
int markdown_core_parser_content_end_place(markdown_core_parser *parser, const markdown_core_content_map *map,
                                           bufsize_t offset, int *line, bufsize_t *end);

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

/* The paragraph `node` an edit's reading reopened at a seam lets go of
 * the children it kept, as it becomes a block of kind `kind`, unless it is
 * the node it was again, of a kind that parses from seams (blocks.c, THE
 * SEAMS). */
void markdown_core_parser_unseam(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node_type kind);

/* The inline content of the root `node`, of a kind the preparation parses
 * that parses as a paragraph does (element.h, `seams`), parsed as
 * completion's walk parses a paragraph's: from its seams when an edit's
 * reading leapt in it (blocks.c, THE SEAMS), whole otherwise. */
void markdown_core_parser_parse_root(markdown_core_parser *parser, markdown_core_node *node);

/* THE LEAP A ROOT TOOK: whether the root `node` an edit's reading parsed
 * from its seams has the measures its element takes of its children
 * (element.h, the finish's `measure`) as its parse made them. Its children
 * before `*from` and from `*to` on are those it kept, which measure `*front`
 * and `*back`, and those between are those the parse made. */
typedef struct {
    markdown_core_measure front, back;
    uint32_t from, to;
} markdown_core_leapt;
bool markdown_core_parser_leapt(const markdown_core_parser *parser, const markdown_core_node *node,
                                markdown_core_leapt *leapt);

/* THE BUILDER STACK'S QUESTIONS (the builders, above), each answered from
 * one builder's scaffold. The current block. */
static inline markdown_core_node *markdown_core_parser_current(const markdown_core_parser *parser) {
    return parser->at.current;
}

/* Whether a block that closed has settled: every one has but the pending
 * block, which a later line can still write. */
static inline bool markdown_core_parser_settled(const markdown_core_parser *parser, const markdown_core_node *node) {
    return node != parser->at.pending;
}

/* A block an element installs in a field, which holds as a settled block
 * does from then on (blocks.c, A BLOCK SETTLES): the document's metadata,
 * which its prefix makes, and an inline note, which its Citation holds. */
static inline void markdown_core_parser_settle_field(const markdown_core_parser *parser, markdown_core_node *node) {
    (void)parser;
    node->flags |= MARKDOWN_CORE_NODE__COMMITTED;
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

/* A LINE'S TAIL OFFERED (element.h, MARKDOWN_CORE_FOLLOW_TAIL): bytes `from`
 * to `to` of `line`, the end of a line of `block` its own grammar leaves
 * open, to the element that takes the tails of its kind; how many bytes
 * before `to` it took, 0 when none does. The fields the taker reads in it
 * go where `fields` has them go. */
bufsize_t markdown_core_parser_offer(markdown_core_parser *parser, markdown_core_node *block, const unsigned char *line,
                                     bufsize_t from, bufsize_t to, bool live, const markdown_core_field_to *fields);

/* Whether a block of `node`'s kind changing to `kind` keeps a block: one
 * whose element cuts the items its text opens with, becoming one of a kind
 * that does not, has them cut first (blocks.c, THE FRONT CUT). False when
 * they were all of it, or the parse failed. */
bool markdown_core_parser_kind_cut(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent,
                                   markdown_core_node_type kind);
/* A block's change of kind: a block whose element cuts the items its text
 * opens with, becoming one of a kind that does not, has them cut first
 * (blocks.c, THE FRONT CUT), and keeps its kind when they were all of it. */
static inline markdown_core_node_set_kind_result markdown_core_parser_set_node_kind(markdown_core_parser *parser,
                                                                                    markdown_core_node *node,
                                                                                    markdown_core_node *parent,
                                                                                    markdown_core_node_type kind) {
    if (parser->dialect && kind != node->kind && parent && !markdown_core_parser_kind_cut(parser, node, parent, kind)) {
        return parser->error ? MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED : MARKDOWN_CORE_NODE_SET_KIND_CUT;
    }
    markdown_core_parser_note_kind(parser, kind);
    if (kind != node->kind) {
        /* What its inline runs gave up was its former kind's reading. */
        markdown_core_node_drop_opens(node, false, 0, 0);
    }
    if (parser->stream && node == parser->stream->reopened && kind != node->kind) {
        markdown_core_parser_unseam(parser, node, kind);
    }
    return markdown_core_node_set_kind(node, parent, kind);
}

/* Physical geometry is present for every visited line. */
typedef struct markdown_core_input_line {
    /* Input buffers, hence offsets and line counts, are bounded by INT32_MAX / 2. */
    uint32_t start, end;
    /* Where the line's own bytes begin: the offset in the line where the
     * bytes of the last open block the line reached begin, past the
     * prefixes of the containers above it, as the block parser last matched
     * them, or the lookahead of the call that claimed the line
     * (markdown_core_parser_claim; markdown_core_parser_line_places). */
    uint32_t own;
    /* How far past its start the looks that began on the line read, of the
     * readings that began no block's first line here (blocks.c, THE BLOCK
     * OF A LINE): 0 for none; and, in the top bit (MARKDOWN_CORE_LINE_CUT),
     * whether a look that began on a line before stopped inside this one,
     * past its first byte and before its end, as the opener of the line's
     * first block decides from what came before (blocks.c, THE CUTS). */
    uint32_t look;
    /* How far past its start the line's reading read as it gave the rest
     * of the line to the block that takes it as text, the reader's `far`
     * there (blocks.c, A REST INSIDE A LINE): 0 for a line that gave
     * none; and, in the top bit (MARKDOWN_CORE_LINE_PAST), whether it
     * looked past the line's end before. */
    uint32_t decided;
} markdown_core_input_line;

#define MARKDOWN_CORE_LINE_CUT UINT32_C(0x80000000)
#define MARKDOWN_CORE_LINE_LOOK(line) ((line)->look & ~MARKDOWN_CORE_LINE_CUT)
#define MARKDOWN_CORE_LINE_PAST UINT32_C(0x80000000)

/* What a lookahead learned of a line it visited (blocks.c, THE LOOKAHEAD):
 * the deepest container of its path it matched there, `container`, NULL for
 * none, at `depth` in the path, and the line's state past that container's
 * prefix. */
typedef struct markdown_core_line_memo {
    const struct markdown_core_node *container;
    int depth;
    /* The line state after that container's prefix: what S_advance_offset left. */
    bufsize_t offset;
    bufsize_t column;
    bool split;
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
    size_t run_end_at;
} markdown_core_line_memo;
/* Where the line after `line`, a line the scan has passed, starts: past
 * its terminator. The lines are in leaves (tree.h), so the next line's
 * record may be in another; the terminator's one or two bytes say it. */
static inline size_t markdown_core_input_line_next(const markdown_core_parser *parser,
                                                   const markdown_core_input_line *line) {
    /* The line's terminator, as the scan read it (S_scan_line), which has
     * scanned past it: a CR, an LF or both, or none at the end. */
    const unsigned char *source = parser->input.source;
    const size_t size = parser->input.scanned;
    size_t at = line->end;
    if (at < size && source[at++] == '\r' && at < size && source[at] == '\n') {
        at++;
    }
    return at;
}

/* What the input's lines are to their tree (blocks.c, THE LINE TREE). */
extern const markdown_core_tree_ops markdown_core_input_lines;

/* Input line `index`, below the lines' count, in place. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_input_line *markdown_core_input_line_at(const markdown_core_parser_input *input, size_t index) {
    return (markdown_core_input_line *)markdown_core_tree_at(&input->lines, index, sizeof(markdown_core_input_line));
}

/* Returned pointers are borrowed until the next request that grows the
 * index. Keep line numbers or copies across such a request. */
markdown_core_input_line *markdown_core_parser_extend_source_lines(markdown_core_parser *parser, size_t index);

static inline markdown_core_input_line *markdown_core_parser_source_line(markdown_core_parser *parser, int line) {
    if (line < parser->input.first_line || parser->error) {
        return NULL;
    }
    size_t index = (size_t)(line - parser->input.first_line);
    return index < parser->input.lines.front ? markdown_core_input_line_at(&parser->input, index)
                                             : markdown_core_parser_extend_source_lines(parser, index);
}

/* THE TEXT OF THE ACTIVE INPUT read so far, through `*length`: a reading of
 * it to its end asks markdown_core_parser_input_ends there. The document's
 * prefix reads it (element.h, markdown_core_document_ops, `read`). */
static inline const unsigned char *markdown_core_parser_input_text(const markdown_core_parser *parser, size_t *length) {
    *length = parser->input.size;
    return parser->input.source;
}

/* THE END OF THE ACTIVE INPUT, learned in one place. Every reading that
 * asks whether the input goes on past a position asks here, or asks for a
 * line after the ones the driver reached (markdown_core_parser_source_line);
 * nothing else compares a position with the input's end. `cursor` is a
 * position of the active input. */
static inline bool markdown_core_parser_input_ends(const markdown_core_parser *parser, const unsigned char *cursor) {
    assert(cursor >= parser->input.source && cursor <= parser->input.source + parser->input.size);
    return cursor == parser->input.source + parser->input.size;
}

/* The reader's reads past its line (reader.h, READS PAST THE LINE). A read
 * of the active input from `from` to `to` that meets the line in hand reads
 * its bytes there, which `far` notes, up to its line ending: a read of that,
 * or of what follows, is a look past the line (blocks.c, A REST INSIDE A
 * LINE). Where a column of the input may stand elsewhere, it notes the
 * whole line. Only a reader that `rests` notes them: a parse that takes no
 * edits has no rest to find inside a line. */
static inline void markdown_core_reader_reads(markdown_core_reader *reader, const markdown_core_parser *parser,
                                              size_t from, size_t to) {
    reader->past = to > reader->past ? to : reader->past;
    if (!reader->rests) {
        return;
    }
    const size_t start = (size_t)reader->start, own = (size_t)reader->line.len - 1;
    if (parser->input.mapped && to > start) {
        reader->far = reader->line.len + 1;
    } else if (to > start && from < start + own) {
        const size_t far = to - start < own ? to - start : own;
        reader->far = (bufsize_t)far > reader->far ? (bufsize_t)far : reader->far;
    }
}

static inline int markdown_core_reader_byte(markdown_core_reader *reader, markdown_core_parser *parser, size_t place) {
    markdown_core_reader_reads(reader, parser, place, place + 1);
    return markdown_core_parser_input_ends(parser, parser->input.source + place) ? -1 : parser->input.source[place];
}

/* The active input's bytes from its start up to `to`, which it holds: the
 * reader notes it read through them. */
static inline const unsigned char *markdown_core_reader_text(markdown_core_reader *reader, markdown_core_parser *parser,
                                                             size_t to) {
    assert(to == 0 || !markdown_core_parser_input_ends(parser, parser->input.source + to - 1));
    markdown_core_reader_reads(reader, parser, 0, to);
    return parser->input.source;
}

static inline size_t markdown_core_reader_after(const markdown_core_reader *reader,
                                                const markdown_core_parser *parser) {
    (void)reader;
    size_t size;
    const unsigned char *source = markdown_core_parser_input_text(parser, &size);
    return parser->at.lookahead_cursor ? (size_t)(parser->at.lookahead_cursor - source) : size;
}

/* WHERE THE LINES THE TEXT HAS FINISHED END (`complete`): the end of the
 * last line with a line ending. The memo a lookahead keeps for a run of
 * blank lines holds finished lines only. */
static inline const unsigned char *markdown_core_parser_complete_end(const markdown_core_parser *parser) {
    return parser->input.source + parser->input.complete;
}

/* The flags of a container a lookahead matches, as it first matched it. */
typedef struct markdown_core_lookahead_container {
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
 * The containers it matches are its PATH, the builders from the active
 * input's root to the block's parent, root first, `depth` of them. `begin`
 * saves the parser's line state, the flags of each container are kept as a
 * line first matches it, from `touched` on, and `end` restores them; nothing
 * else in the parser or the tree is touched. `begin` returns false only when
 * an allocation failed, and has then marked the parse lost. */
typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_node *const *path;
    const unsigned char *cursor;
    /* The path's length, the first container a line has matched, and the
     * first the line being read matches. */
    int depth, touched, from;
    int line;
    int run_start;
    /* Where the reading stood as `begin` found it, which `end` puts back. */
    markdown_core_parser_at saved;
    bool active;
    /* The reading met the end of the input. */
    bool ended;
    /* The reading reads the next line alone, blank or not: a caller that
     * asks only whether it is blank sets it after `begin`, and what it
     * decided depends on no line past it. */
    bool once;
    /* The instance whose opener the reading is, when its element has stop
     * lines (element.h, `stops`), and whether the reading found one: set
     * by markdown_core_parser_lookahead_opener before the end. */
    bool closed;
    const struct markdown_core_element_instance *opener;
} markdown_core_block_lookahead;

/* The reading is one of `owner`'s opener, which `closed` says whether it
 * found a stop line for (element.h, `stops`): one in the document's root
 * that found none notes its opener as it ends, and not what it read
 * (blocks.c, AN OPENER READ TO THE END). */
static inline void markdown_core_parser_lookahead_opener(markdown_core_block_lookahead *lookahead,
                                                         const struct markdown_core_element_instance *owner,
                                                         bool closed) {
    lookahead->opener = owner;
    lookahead->closed = closed;
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
/* The answers for a line that is not a copy of its source: a line of a
 * cell's content. */
bufsize_t markdown_core_parser_mapped_source_offset(markdown_core_parser *parser, int line, int column);
bufsize_t markdown_core_parser_mapped_source_end(markdown_core_parser *parser, int line, int column);

/* The geometry of input line `line`, which the driver has already visited. */
static inline markdown_core_input_line *markdown_core_parser_visited_line(const markdown_core_parser *parser,
                                                                          int line) {
    size_t index = (size_t)(line - parser->input.first_line);
    assert(line >= parser->input.first_line && index < parser->input.lines.front);
    return markdown_core_input_line_at(&parser->input, index);
}

/* The same, to change in place. */
static inline markdown_core_input_line *markdown_core_parser_rewritten_line(markdown_core_parser *parser, int line) {
    return markdown_core_parser_visited_line(parser, line);
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
/* The content of line `line` of the active input, which the driver read,
 * as the block parser reads it, without its terminator, and
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

/* PLACES ON THE LINE IN HAND (reader.h): where the line's bytes `from` up
 * to `to` were written -- the document's bytes, or, on a line of a cell's
 * content, the bytes of the cell they copy. Placing is no read: it notes
 * nothing. */
static inline markdown_core_place markdown_core_reader_span(const markdown_core_reader *reader,
                                                            markdown_core_parser *parser, bufsize_t from,
                                                            bufsize_t to) {
    markdown_core_place place;
    place.start = (uint32_t)markdown_core_parser_source_offset(parser, reader->number, (int)from + 1);
    place.end = (uint32_t)markdown_core_parser_source_end(parser, reader->number, (int)to);
    return place;
}
/* `node`'s text, from its byte `offset`, holds the line's `length` bytes
 * from `from`: its map places them where they were written. 0, with the
 * parse failed, when storage could not be had. */
static inline int markdown_core_reader_marks(const markdown_core_reader *reader, markdown_core_parser *parser,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length,
                                             bufsize_t offset) {
    return markdown_core_parser_append_line_marks(parser, node, reader->number, reader->start, (int)from + 1, length,
                                                  offset);
}

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, struct markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead);
/* The next non-blank line that carries the prefixes, as the reader on it
 * (reader.h): its bytes through its line ending, its number, the offset
 * past the prefixes, its first non-space byte and its indentation after
 * them; and the blank lines stepped over before it. NULL when the
 * lookahead has ended. The reader is the parser's: the lookahead's end
 * puts it back on the line in hand. */
markdown_core_reader *markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, int *blank_lines);
void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead);

/* THE DEFINITION (labels.h): `node`, a definition, defines `label` in
 * `kind`'s space from `start` on, in the space's form, as the lookup above
 * reads it: a reference or footnote label normalized into `*normal`, in
 * storage the node that holds it owns, a specimen label as written, when
 * `normal` may be NULL. The node owns the definition, which goes when
 * it does. An empty label defines nothing, and the document lists the
 * node all the same (labels.h, THE SEQUENCES).
 * False, with the parse failed, when storage could not be had. */
bool markdown_core_parser_declare(markdown_core_parser *parser, markdown_core_label_space kind,
                                  const markdown_core_chunk *label, uint32_t start, markdown_core_chunk *normal,
                                  markdown_core_node *node);
/* The byte of the text that offset `at` of the content of the block being
 * completed reads (its inline root's offset,
 * markdown_core_inline_state_root_offset), as a use's is (blocks.c, WHERE
 * A USE IS): where a definition its inline content makes starts. */
uint32_t markdown_core_parser_source_at(markdown_core_parser *parser, uint32_t at);
/* Whether `kind`'s labels are in the reference-label normal form (element.h,
 * markdown_core_space `folds`); the engine's ANCHOR's are not. */
static inline bool markdown_core_parser_folds(const markdown_core_parser *parser, markdown_core_label_space kind) {
    const markdown_core_space *space = parser->dialect->spaces[kind];
    return space && space->folds;
}
/* THE DEFINITION LOOKUP of inline content: the record of `kind` that
 * declares `label`, as written, for the block being completed, or NULL.
 * Reference and footnote labels are looked up in their normal form, and a
 * specimen label as its bytes (labels.h): every definition answers. `at`
 * is where the lookup is in the content of the block being completed (its
 * inline root's offset, markdown_core_inline_state_root_offset), where the
 * lookup is a reference to the label by the root (labels.h). `*failed`
 * is set when storage could not be had, and the answer is then NULL. */
const markdown_core_mention *markdown_core_parser_look_up(markdown_core_parser *parser, markdown_core_label_space kind,
                                                          const markdown_core_chunk *label, uint32_t at, bool *failed);
static inline const markdown_core_mention *markdown_core_parser_find_definition(markdown_core_parser *parser,
                                                                                markdown_core_label_space kind,
                                                                                const markdown_core_chunk *label,
                                                                                uint32_t at, bool *failed) {
    return markdown_core_parser_look_up(parser, kind, label, at, failed);
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
/* A PARSE IS A START, A COMMIT AND A FINALIZE, one transaction at a time on
 * an instance. `start` begins a transaction that continues what `revision`
 * says, with the storage it lends (above). `commit` reads every line of
 * `source`, the whole text, which it borrows until the end of the
 * transaction; an edit's reading commits the new text once the reopen made
 * room for it (blocks.c, THE REOPEN). False when the transaction failed.
 * `finalize` ends the input there: every block still open closes, the
 * closed blocks complete, and the tree is published and returned, or NULL
 * when the transaction failed. `discard` ends a transaction and returns
 * nothing. Both release the transaction's state, so an instance runs any
 * number of them, each as the first. `flush` is `finalize` without the
 * release: the state stays, for a session's edits, until a `discard`
 * releases it. */
void markdown_core_parser_start(markdown_core_parser *parser, markdown_core_revision *revision);
bool markdown_core_parser_commit(markdown_core_parser *parser, const char *source, size_t length);
markdown_core_node *markdown_core_parser_finalize(markdown_core_parser *parser);
markdown_core_node *markdown_core_parser_flush(markdown_core_parser *parser);
void markdown_core_parser_discard(markdown_core_parser *parser);
/* THE FOLD MARKS of a block of the chain a reading reopens (blocks.c,
 * S_reopen_level): `flags[c]`, for each of its first `count` chunks of
 * MARKDOWN_CORE_FOLD_CHUNK children, its flags as its children through the
 * chunk's last folded into it, from a reopen with none. Its children
 * fold only the bits its kind folds (S_fold), all clear before the
 * first, so its reopen folds the kept children from the last mark before
 * the last of them, not from its first. The caller keeps them while the
 * children they mark stand as they are. */
#define MARKDOWN_CORE_FOLD_CHUNK 64

/* THE FOLD PASSES of the same block: for each chunk of its children, up
 * to two of the bits its kind folds, `from`, which its children fold to
 * `to` as they fold into it one after another, `count` of them, none when
 * the chunk's children are not those it was asked for (blocks.c,
 * S_fold_old). The caller clears a chunk's as its children change, and
 * all from one on as children move. */
typedef struct markdown_core_fold_pass {
    markdown_core_node_internal_flags from[2], to[2];
    uint32_t count;
} markdown_core_fold_pass;

typedef struct markdown_core_folds {
    markdown_core_node_internal_flags *flags;
    size_t count, capacity;
    markdown_core_fold_pass *passes;
    size_t pass_count, pass_capacity;
} markdown_core_folds;

/* A flushed transaction put back at input line `line`, a rest point of
 * the flushed document `root` before its child `child`, to read the new
 * text `source` of an edit that replaced the old text from `from` to `to`
 * and moved what follows by `delta`: a commit that ends the text and a `flush` read
 * it from there (blocks.c, THE REOPEN), and take the old children after the
 * edit back where they read as they did (THE SPLICE), unless `whole` says
 * to read all of it, and none before `past`, an offset of the new text,
 * either. `prior` is where the child before `child` ends, or the
 * root starts. With `depth`, the rest point is below the root (THE NESTED
 * REOPEN): the child before `child` and the `depth - 1` blocks `path`
 * names below it are reopened, the deepest at its child `path[depth]`,
 * each folding its kept children from its `folds`, when they are not
 * NULL, and each where `places` says. With `mid` other than SIZE_MAX, the
 * reading stands inside the line there, where the text of the chain's last
 * block begins (A REST INSIDE A LINE). False, with the transaction failed, when storage could
 * not be had. */
struct markdown_core_index;
bool markdown_core_parser_reopen(markdown_core_parser *parser, markdown_core_node *root, uint32_t child, size_t line,
                                 uint32_t prior, size_t from, size_t to, int64_t delta, size_t past, const char *source,
                                 bool whole, const uint32_t *path, const markdown_core_chain_place *places,
                                 size_t depth, size_t mid, markdown_core_folds *const *folds,
                                 const struct markdown_core_index *index);
/* THE COMPACTION (plan 12.4): after a reading of the whole text, gives back
 * the storage the input and the reading hold past what the text needs, as
 * the most text before left it. */
void markdown_core_parser_compact(markdown_core_parser *parser);
/* Whether `block` is a leaf a rest point may be in: a paragraph or a block
 * of literal lines, or one that read its lines as such a kind (blocks.c,
 * THE LEAP, THE FORMER KIND). */
bool markdown_core_parser_leaf(const markdown_core_parser *parser, const struct markdown_core_node *block);
/* The bytes of the `length` at `bytes` before the first line ending,
 * all of them when none ends a line. */
static inline size_t markdown_core_line_end(const unsigned char *bytes, size_t length) {
    const unsigned char *feed = length ? (const unsigned char *)memchr(bytes, '\n', length) : NULL;
    const size_t end = feed ? (size_t)(feed - bytes) : length;
    const unsigned char *ret = end ? (const unsigned char *)memchr(bytes, '\r', end) : NULL;
    return ret ? (size_t)(ret - bytes) : end;
}
/* THE BLOCK BEFORE (index.h, THE REST; blocks.c, THE SPLICE): whether a
 * line of the `length` bytes at `lines`, read after `prior`, may write it:
 * a block of a kind a later line may write once it closed (element.h, the
 * follow's `closed`), when the follow's element says the line may
 * (`writes`), or any line when it says nothing. */
bool markdown_core_parser_writes(const markdown_core_parser *parser, const struct markdown_core_node *prior,
                                 const unsigned char *lines, size_t length);
/* Whether an edit of a later line of `block`, a leaf, may rest at that line
 * and leap past its old lines (blocks.c, THE LEAP). */
bool markdown_core_parser_leaps(const markdown_core_parser *parser, const struct markdown_core_node *block);
/* Whether `block` reads literal lines: a block of a kind of them, or one
 * that read them as such a kind before it closed as its own (element.h,
 * THE FORMER KIND), which an edit's reading reopens as that kind. */
bool markdown_core_parser_reads_lines(const markdown_core_parser *parser, const struct markdown_core_node *block);
/* Whether the children of `block` abut (element.h, `abuts`). */
bool markdown_core_parser_abuts(const markdown_core_parser *parser, const struct markdown_core_node *block);
/* How the line before the rest point line `line` reads to the `depth`
 * blocks of `chain` reopened there (blocks.c, THE NESTED REOPEN), in the
 * text `text`, when the child before the rest point ends at `before`: blank
 * to the blocks from the one returned down, none when that is `depth`.
 * SIZE_MAX when the rest point has none: the child before ends on that line
 * or later, as it closed when the rest point's line was read, unless the
 * deepest block's children abut, or the line is no blank one the deepest
 * block took, or holds prefixes alone and which of the chain took them is
 * not known. */
size_t markdown_core_parser_rest_blank(const markdown_core_parser *parser, const unsigned char *text,
                                       struct markdown_core_node *const *chain, size_t depth, size_t line,
                                       size_t before);
/* Where the last reading of an edit took the old children back, in the new
 * text: 0 when it took none back. */
size_t markdown_core_parser_resumed(const markdown_core_parser *parser);
/* The root's child the old children it took back begin at, when it took
 * some back. */
uint32_t markdown_core_parser_tail(const markdown_core_parser *parser);
/* The child of the chain's block `level` the old children it took back
 * begin at, or UINT32_MAX when it took none back into that block. */
uint32_t markdown_core_parser_tail_in(const markdown_core_parser *parser, size_t level);
/* THE USES OF ALTERED LABELS: one past where each reference (labels.h)
 * of a label the last transaction changed (THE ALTERED LABELS) lies whose
 * answer is not the label's now, in order and each once, in `*places`,
 * which the caller frees: the lookups that would answer otherwise than
 * they did, and no other. False when storage could not be had. */
bool markdown_core_parser_altered_uses(markdown_core_parser *parser, size_t **places, size_t *place_count);
/* Where the text an edit's back is measured from ends (THE BACK). */
static inline uint32_t markdown_core_parser_back_end(const markdown_core_parser *parser) {
    return (uint32_t)parser->input.back_end;
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
/* Whether the parse notes what its readings read (THE REACH): one that
 * takes edits, outside a quiet lookahead. A parse that takes no edits pays the test and
 * nothing more. */
static inline bool markdown_core_parser_reaches(const markdown_core_parser *parser) {
    return parser->stream && parser->stream->edit && !parser->stream->edit->quiet;
}
/* A reading of the edit read to `end` (markdown_core_edit, `ran`). */
static inline void markdown_core_parser_ran(markdown_core_parser *parser, size_t end) {
    markdown_core_edit *edit = parser->stream->edit;
    edit->ran = end > edit->ran ? end : edit->ran;
}
/* Where a reading of the line in hand ends, in a parse that notes reach
 * (THE REACH: an end is past the last byte read): past its line ending,
 * or past the text's end when it met it, a line with no ending or no line
 * at all as the input ends, as what follows the text may go on with what
 * it read. On the line the splice closes blocks of the chain on, as their
 * old reading did (blocks.c, S_close_inside). */
size_t markdown_core_parser_read_past(const markdown_core_parser *parser);
/* THE TWO KINDS OF READING (blocks.c, THE REACH): a LOOK read the bytes up
 * to its end and decided from them, and what it decided stands while they
 * do; a SPAN is a block's reading held open from its start to its end, as
 * a block's close notes it, and no reading starts afresh inside it, the
 * bytes there changed or not. */
/* `node` read from `start` to `end`, a span or a look: what its reading
 * read (node.h, `opened`, `closed`, `look`). */
static inline void markdown_core_node_reads(markdown_core_node *node, size_t start, size_t end, bool span) {
    const size_t at = node->place.start;
    if (span && start < at && at - start > node->opened) {
        node->opened = (uint32_t)(at - start);
    }
    const uint32_t past = end > at ? (uint32_t)(end - at) : 0;
    uint32_t *field = span ? &node->closed : &node->look;
    if (past > *field) {
        *field = past;
    }
}

/* A look of the reading in hand that names no block, which the block of
 * its line takes as the line ends (blocks.c, THE REACH). */
void markdown_core_parser_line_look(markdown_core_parser *parser, size_t start, size_t end);
/* A look from `start` to `end` that stopped inside a later line marks it
 * (blocks.c, THE CUTS). */
void markdown_core_parser_look_cut(markdown_core_parser *parser, size_t start, size_t end);
/* A look of `node`'s from `start`, before it, which goes to the block of
 * its line when that is a line before the node's (blocks.c, THE BLOCK OF A
 * LINE): a look's reading begins on the line it is the look of. */
void markdown_core_parser_look_before(markdown_core_parser *parser, markdown_core_node *node, size_t start, size_t end);
/* `node` starts at `start` from now on, and what its reading read stays
 * where it was: its spans from where they began (`opened`), and its look,
 * which began where it started, the block of that line's (THE BLOCK OF A
 * LINE). */
static inline void markdown_core_node_restart(markdown_core_parser *parser, markdown_core_node *node, size_t start) {
    const size_t old = node->place.start;
    const int64_t moved = (int64_t)start - (int64_t)old;
    const size_t look = node->look ? old + node->look : 0;
    if (node->opened || node->closed) {
        const int64_t opened = (int64_t)node->opened + moved;
        node->opened = opened > 0 ? (uint32_t)opened : 0;
    }
    node->closed = (int64_t)node->closed > moved ? (uint32_t)((int64_t)node->closed - moved) : 0;
    node->look = 0;
    node->place.start = (uint32_t)start;
    if (look) {
        markdown_core_parser_look_before(parser, node, old, look);
    }
}
/* `node`'s reading read from `start` to `end`, a look, or a span for
 * markdown_core_parser_reach_span; NULL for the block of the line in hand. */
static inline void markdown_core_parser_reach(markdown_core_parser *parser, markdown_core_node *node, size_t start,
                                              size_t end) {
    if (markdown_core_parser_reaches(parser) && end > start) {
        markdown_core_parser_ran(parser, end);
        markdown_core_parser_look_cut(parser, start, end);
        if (node && start < node->place.start) {
            markdown_core_parser_look_before(parser, node, start, end);
        } else if (node) {
            markdown_core_node_reads(node, start, end, false);
        } else {
            markdown_core_parser_line_look(parser, start, end);
        }
    }
}
static inline void markdown_core_parser_reach_span(markdown_core_parser *parser, markdown_core_node *node, size_t start,
                                                   size_t end) {
    if (markdown_core_parser_reaches(parser) && end > start) {
        markdown_core_parser_ran(parser, end);
        markdown_core_node_reads(node, start, end, true);
    }
}
/* AN OPENER READ TO THE END (blocks.c): the reading of `owner`'s opener
 * on the line that starts at `start`, in the document's root, read to the
 * text's end and found no stop line (element.h, `stops`); and, for
 * `reach_shut`, it found one, and what it read is in the reach. A parse
 * that takes no edits pays the test and nothing more. The parse fails when storage could not be had. */
void markdown_core_parser_reach_open(markdown_core_parser *parser, const struct markdown_core_element_instance *owner,
                                     size_t start);
void markdown_core_parser_reach_shut(markdown_core_parser *parser, const struct markdown_core_element_instance *owner,
                                     size_t start);
/* The openers of the flushed document read to the end, `*count` of them,
 * in no order. */
const markdown_core_unclosed *markdown_core_parser_openers(const markdown_core_parser *parser, size_t *count);
/* The first of the first `lines` lines whose looks read past `past`
 * (markdown_core_input_line, `look`), or `lines`. */
size_t markdown_core_parser_line_looked(const markdown_core_parser *parser, size_t lines, size_t past);
bool markdown_core_parser_line_span(const markdown_core_parser *parser, size_t line, size_t *start, size_t *end);
/* Where the own bytes of line `line` of the flushed text begin, past the
 * prefixes of the containers above its last open block (`own`). */
size_t markdown_core_parser_line_own(const markdown_core_parser *parser, size_t line);
/* Where the reading of line `line` of the flushed text had read to as it
 * gave the rest of the line to the block that takes it as text, one past
 * the furthest byte (`decided`), or 0 when it gave none or the text has no
 * such line; and whether it looked past the line's end before, `*past`,
 * unless that is NULL. */
size_t markdown_core_parser_line_decided(const markdown_core_parser *parser, size_t line, bool *past);
/* How many lines the input has, the empty one after a last line ending
 * counted, which markdown_core_parser_line_span answers for. */
size_t markdown_core_parser_line_count(const markdown_core_parser *parser);
void markdown_core_parser_destroy(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
