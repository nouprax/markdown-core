#ifndef MARKDOWN_CORE_INLINE_INTERNAL_H
#define MARKDOWN_CORE_INLINE_INTERNAL_H
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown_core_ctype.h"
#include "utf8.h"
#include "inlines.h"
#include "element.h"

typedef struct markdown_core_field_read markdown_core_field_read;
struct markdown_core_inline_state {
    markdown_core_chunk input;
    /* Where a cut the owner's text gave a take that its inlines may yet give
     * back begins (element.h, markdown_core_follow, `yields`), or -1: text yields
     * there, a token that starts there ends the run, and one that crosses it
     * gives the cut back. */
    bufsize_t cut;
    /* THE NODE A CONSTRUCT MADE THAT A LATER ONE MAY UPDATE (no-flush.md
     * 10.13): its kind, and where it ends, as the reading of the text says
     * (markdown_core_text_made). */
    bufsize_t made_end;
    markdown_core_node_type made_kind;
    /* The readings of the fields the run's tokens hold, made as the run is
     * read and compiled as its compile reaches the token
     * (markdown_core_text_field); NULL once one is compiled. */
    markdown_core_field_read **reads;
    size_t read_count, read_room;
    bufsize_t pos;
    /* The block whose content buffer `input` is, and the parser that holds
     * requirement 10's content-to-source map for it. Both are NULL for a
     * inline state built straight out of a chunk -- the reference-definition
     * parser -- and the map is then simply not consulted. */
    markdown_core_parser *owner_parser;
    markdown_core_node *owner;
    /* The instance of `owner`'s structure element, resolved once. The projection is a pure
     * function of `owner->kind`, `owner` does not change across a run, and a
     * run's owner does not change kind during it -- so asking per token was
     * asking the same question once per inline node. Written only beside
     * `owner`, in the one place that assigns it, so the pair cannot drift. */
    const markdown_core_element_instance *owner_structure;
    /* The run of `owner`'s map the last placement ended in, and the frame
     * the next is measured in: the inline parser reads `input` left to right,
     * so a node that lies whole on this run is placed by arithmetic, and one
     * that leaves it is resolved by probing from here (see
     * `markdown_core_inline_state_place`). Seeded on the owner's first run. */
    int mark_cursor;
    /* THE FRAME THE CURSOR'S RUN GIVES A PLACEMENT, written with the cursor
     * (markdown_core_inline_seat_cursor) and read by every placement: the
     * run's first content offset, the offset the next run starts at (past
     * every offset when the cursor is the owner's last run), and the run's
     * line, source byte, step and width. A node whose two ends lie in the frame
     * is placed by arithmetic on these six values and reads nothing from the
     * map; `mapped` is whether there is a map at all -- a state built
     * straight out of a chunk, the reference-definition parser's, has none,
     * and neither has a block that came with no content. */
    bufsize_t mark_run_start, mark_run_end;
    int mark_line, mark_step, mark_width;
    bufsize_t mark_source;
    bool mapped;
    /* How many runs of each rule the reading holds that can open, and how
     * many that can close (attention.h, markdown_core_attention_count), so
     * a scanner can ask whether a closer will pair without walking them. */
    int delim_openers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int delim_closers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    /* Whether no link may open where the reading stands: a link's own
     * text forbids one until a new label opens (label.h). */
    bool no_link_openers;
    /* This run's block of the elements' run records (markdown_core_run_state):
     * taken from the parser when the run starts and given back when it is
     * cleared. NULL for a run with no parser, and for one whose records could
     * not be allocated, which is then failed and never begins. */
    unsigned char *run_state;
    /* The owning parser's sealed dialect, which the scan reads its byte
     * tables and delimiter owners from. NULL with no parser: that state is a
     * reference definition's cursor (link.c, S_reference_definition),
     * which reads no table, as it pushes no delimiter. */
    const markdown_core_dialect *dialect;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    markdown_core_parse_error error;
    /* THE END READS (docs/plans/2026-09-29-incremental-parsing.md, 5.8):
     * where the top-level token being read starts, and the earliest such
     * start whose reading read the end of `input` -- asked for a byte at or
     * past it, or found what it looked for only there -- or INT32_MAX while
     * none has. What a token decided before its reading reached the end
     * stays decided however the input goes on. */
    bufsize_t token, undecided;
    /* The token starts a settle point may be (markdown_core_inline_settle),
     * or NULL when the run's caller asks for none. */
    markdown_core_inline_settle *settle;
    /* The old seams the run stops at, or NULL (markdown_core_seam_stops). */
    markdown_core_seam_stops *stops;
};

/* Whether an end read of the token being read would be one the run lists:
 * no earlier token read the end, and the run lists settle starts. A check
 * whose only use is to say a token read the end asks this first. */
static inline bool markdown_core_inline_tracks_end(const markdown_core_inline_state *inline_state) {
    return inline_state->token < inline_state->undecided;
}

/* THE OPEN CONSTRUCT a token gave up (node.h, OPENER and OPEN): it opened a
 * construct of kind `opens` (NONE when that is still open, as a `[` that
 * may open a link, a span or a cite) whose opening syntax is content bytes
 * `from` up to `to`, and wrote the `count` fields at `fields`, which may be
 * none, as far as its reading read them. The run's node keeps them, which
 * the completion reads at a cursor as it reads a finished node's fields. */
void markdown_core_inline_open(markdown_core_inline_state *inline_state, markdown_core_node_kind opens, bufsize_t from,
                               bufsize_t to, const markdown_core_field *fields, size_t count);
/* The inline note `note`, a field its Citation holds, settles as a field
 * does (markdown_core_parser_settle_field), and a run that lists settle
 * starts lists it, so it commits with the node that holds it as that
 * settles (blocks.c, THE SETTLE POINT). */
void markdown_core_inline_settle_note(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                      markdown_core_node *note);

/* The token being read read the end of the input. */
static inline void markdown_core_inline_saw_end(markdown_core_inline_state *inline_state) {
    if (inline_state->token < inline_state->undecided) {
        inline_state->undecided = inline_state->token;
    }
}

/* The run record of `self` (markdown-core-element-api.h, "AN ELEMENT AS ONE
 * PARSE HOLDS IT"), or NULL when the run has none. The record lives until the
 * run is cleared. */
static inline void *markdown_core_run_state(const markdown_core_inline_state *inline_state,
                                            const markdown_core_element_instance *self) {
    return inline_state->run_state ? inline_state->run_state + self->run_offset : NULL;
}

#define make_str(inline_state, sc, ec, s)                                                                              \
    markdown_core_inline_make_literal(inline_state, (inline_state)->dialect->plain, sc, ec, s)

/* Read the cursor's run into the frame (inline_internal.h, `mark_run_start`
 * and the fields after it). Called wherever the cursor is written: seeded on
 * the owner's first run when a parse starts, and moved by the span when a
 * placement leaves the frame. The cursor is always a run of the owner -- the
 * seed and the span both name one -- which is the invariant the frame rests
 * on; it is asserted here rather than tested per placement. */
static inline void markdown_core_inline_seat_cursor(markdown_core_inline_state *inline_state) {
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_node *owner = inline_state->owner;
    inline_state->mapped = parser && owner && owner->content_map.count > 0;
    if (!inline_state->mapped) {
        return;
    }
    int cursor = inline_state->mark_cursor, last = owner->content_map.first + owner->content_map.count - 1;
    assert(cursor >= owner->content_map.first && cursor <= last);
    const markdown_core_line_mark *mark = &parser->line_marks.values[cursor];
    inline_state->mark_run_start = mark->content_offset;
    inline_state->mark_run_end = cursor < last ? parser->line_marks.values[cursor + 1].content_offset : INT32_MAX;
    inline_state->mark_line = mark->line;
    inline_state->mark_source = mark->source;
    inline_state->mark_step = mark->source_step;
    inline_state->mark_width = mark->source_width;
}

/* A Text's map, once its extent is placed on the runs [first, last]. A Text
 * whose bytes ARE the source bytes of its scope takes a view of the source
 * map; a decoded token, or a literal shorter than its scope, maps each of
 * its bytes to the whole authored extent. The common Text is a view of
 * `input` at `from`, which is that fact by identity; an element that placed
 * a copy it made (a citation prefix moved into its own buffer) is asked byte
 * for byte. The writes stay inside the gate: a node that is not a verbatim
 * copy of its source must keep `content_map.count` at zero, because that
 * count is read elsewhere as "is there a mapping at all". */
static inline void markdown_core_inline_map_text(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                                 int from, int to, int first, int last) {
    const markdown_core_chunk *literal = node->as.literal;
    if (literal->len == to - from + 1 &&
        (literal->data == inline_state->input.data + from ||
         memcmp(literal->data, inline_state->input.data + from, (size_t)literal->len) == 0)) {
        node->content_map.first = first;
        node->content_map.count = last - first + 1;
        node->content_map.offset = from + inline_state->owner->content_map.offset;
    } else {
        node->content_map.count = 0;
        node->content_map.offset = 0;
        markdown_core_parser_append_content_mark(inline_state->owner_parser, node, 0, inline_state->mark_line,
                                                 node->place.start, (int)(node->place.end - node->place.start), 0);
    }
}

/* The source place of the bytes `from` up to `end` of the run's text, by
 * its frame's arithmetic (inline_internal.h, markdown_core_inline_place):
 * false, `*place` untouched, when either end lies off the cursor's run.
 * `end` equal to `from` places where `from` starts. */
static inline bool markdown_core_inline_frame_place(const markdown_core_inline_state *inline_state, bufsize_t from,
                                                    bufsize_t end, markdown_core_place *place) {
    if (!inline_state->mapped || from < 0 || end < from) {
        return false;
    }
    const bufsize_t base = inline_state->owner->content_map.offset;
    const bufsize_t first = from + base, last = (end > from ? end - 1 : from) + base;
    if (first < inline_state->mark_run_start || last >= inline_state->mark_run_end) {
        return false;
    }
    const bufsize_t start =
        inline_state->mark_source + (first - inline_state->mark_run_start) * inline_state->mark_step;
    place->start = (uint32_t)start;
    place->end = end > from ? (uint32_t)(inline_state->mark_source +
                                         (last - inline_state->mark_run_start) * inline_state->mark_step +
                                         inline_state->mark_width)
                            : (uint32_t)start;
    return true;
}

/* A placement that leaves the frame: resolved by the span, which moves the
 * cursor to where the node ends (inlines.c). */
void markdown_core_inline_place_outside_frame(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                              int from, int to);

/* GIVE `node` THE SOURCE EXTENT OF THE CONTENT BYTES [from, to]. This is the
 * whole of the inline position model: a position is a PROJECTION of the byte
 * range a node covers, asked of requirement 10's content-to-source map, and
 * not a counter each handler keeps in step. The parser reads `input` left to
 * right and a Text never crosses a line ending, so the node being placed
 * almost always lies whole on the run the previous placement ended in: both
 * of its ends are then the frame's source byte plus a distance. Each end
 * is tested on its own: a span's ends are resolved independently (an empty
 * field is placed as [x, x - 1], and when x is a run's first byte its two
 * ends are on two runs), so a test that bounded `from` from below and `to`
 * from above alone would measure such a span in one run with a distance the
 * run does not contain. A leaf: the path that leaves the frame is a call. */
static inline void markdown_core_inline_place(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                              int from, int to) {
    if (!inline_state->mapped) {
        return;
    }
    bufsize_t base = inline_state->owner->content_map.offset;
    bufsize_t from_offset = from + base, to_offset = to + base;
    inline_state->owner_parser->content_mark_queries++;
    if (from < 0 || to < 0 || from_offset < inline_state->mark_run_start || to_offset < inline_state->mark_run_start ||
        from_offset >= inline_state->mark_run_end || to_offset >= inline_state->mark_run_end) {
        markdown_core_inline_place_outside_frame(inline_state, node, from, to);
        return;
    }
    node->place.start =
        (uint32_t)(inline_state->mark_source + (from_offset - inline_state->mark_run_start) * inline_state->mark_step);
    node->place.end =
        (uint32_t)(inline_state->mark_source + (to_offset - inline_state->mark_run_start) * inline_state->mark_step +
                   inline_state->mark_width);
    if (node->kind == inline_state->dialect->plain && node->as.literal->len > 0) {
        markdown_core_inline_map_text(inline_state, node, from, to, inline_state->mark_cursor,
                                      inline_state->mark_cursor);
    }
}

markdown_core_node *markdown_core_inline_make_literal(markdown_core_inline_state *inline_state,
                                                      markdown_core_node_type t, int start_column, int end_column,
                                                      markdown_core_chunk s);
markdown_core_node *markdown_core_inline_make_simple(markdown_core_inline_state *inline_state,
                                                     markdown_core_node_type t);
markdown_core_node *markdown_core_inline_make_simple_with_state(markdown_core_inline_state *inline_state,
                                                                markdown_core_node_type t);
void markdown_core_inline_state_from_buf(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         markdown_core_chunk *chunk);
/* THE INLINE STREAM: the run's bytes and its cursor, which only the engine
 * holds. An element reads them through these and never through
 * `inline_state->input` or `inline_state->pos`, so that every read of the
 * run's end is the engine's to see: asking past the end IS reading it, and
 * the token being read is then one that read the end
 * (markdown_core_inline_saw_end).
 *
 *   `offset`  -- the cursor.
 *   `seek`    -- moves it, forward past what a token took, or back to an
 *                offset it had (an attempt that failed).
 *   `has`     -- whether the run holds the byte at `at`; false reads the end.
 *   `char_at` -- the byte at `at`, 0 at or past the end, which reads it.
 *   `take`    -- `length` bytes from `from`, as a chunk the node owns.
 *   `window`  -- the bytes from `from` to the end, for a scanner that scans a
 *                bounded window and says whether it met the window's end
 *                (`met_end`); the caller reports that end as read.
 *   `size`    -- the run's length, for a bound that reads no byte (a table
 *                sized to the run), never to test where the run ends.
 *   `shorten` -- ends the run at `end` and drops the white space before it:
 *                a grammar that gives the run's tail to another before the
 *                run is read (an ATX heading's closing sequence). */
/* Where the fields of `node`, read from the run by its offsets, go
 * (markdown_core_field_to): nowhere when the run is placed nowhere. */
static inline markdown_core_field_to markdown_core_inline_fields(const markdown_core_inline_state *inline_state,
                                                                 markdown_core_node *node) {
    const bool placed = inline_state->owner_parser && inline_state->owner && inline_state->owner->content_map.count > 0;
    markdown_core_field_to to = {placed ? inline_state->owner_parser : NULL,
                                 placed ? node : NULL,
                                 placed ? &inline_state->owner->content_map : NULL,
                                 NULL,
                                 0,
                                 inline_state};
    return to;
}
static inline bufsize_t markdown_core_inline_offset(const markdown_core_inline_state *inline_state) {
    return inline_state->pos;
}
static inline void markdown_core_inline_seek(markdown_core_inline_state *inline_state, bufsize_t to) {
    inline_state->pos = to;
}
static inline bool markdown_core_inline_has(markdown_core_inline_state *inline_state, bufsize_t at) {
    if (at < inline_state->input.len) {
        return true;
    }
    markdown_core_inline_saw_end(inline_state);
    return false;
}
static inline unsigned char markdown_core_inline_char_at(markdown_core_inline_state *inline_state, bufsize_t at) {
    return markdown_core_inline_has(inline_state, at) ? inline_state->input.data[at] : 0;
}
static inline markdown_core_chunk markdown_core_inline_take(markdown_core_inline_state *inline_state, bufsize_t from,
                                                            bufsize_t length) {
    return markdown_core_chunk_dup(&inline_state->input, from, length);
}
static inline markdown_core_chunk markdown_core_inline_window(const markdown_core_inline_state *inline_state,
                                                              bufsize_t from) {
    markdown_core_chunk window = {inline_state->input.data + from, inline_state->input.len - from, 0};
    return window;
}
static inline void markdown_core_inline_met_end(markdown_core_inline_state *inline_state) {
    markdown_core_inline_saw_end(inline_state);
}
static inline bufsize_t markdown_core_inline_size(const markdown_core_inline_state *inline_state) {
    return inline_state->input.len;
}
static inline void markdown_core_inline_shorten(markdown_core_inline_state *inline_state, bufsize_t end) {
    inline_state->input.len = end;
    markdown_core_chunk_rtrim(&inline_state->input);
}
unsigned char markdown_core_inline_peek_char_n(markdown_core_inline_state *inline_state, bufsize_t n);
unsigned char markdown_core_inline_peek_char(markdown_core_inline_state *inline_state);
unsigned char markdown_core_inline_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos);
int markdown_core_inline_is_eof(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_spaces(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_line_end(markdown_core_inline_state *inline_state);
void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent, bool root,
                                        markdown_core_inline_state *inline_state);
void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state);
void markdown_core_inline_finish_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
/* Reads the run, as its finish would, unless something it reads is held
 * open waiting on what the document says later (a label, which its rules
 * close by the definitions): then the run is left as it was, to finish
 * once the document is known, and the answer is false. */
bool markdown_core_inline_settles(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
/* THE FIELD OF A TOKEN (plan symbols-cleanroom.md 23.10, fields): `root`,
 * a field root the token being read makes, whose content is text, is read
 * there, as the reading reads the token, so that what it looks up is
 * looked up in order; whether its text has a boundary (an enclosing word
 * body pairs across none) is `*spaced`. The reading holds `root` until
 * compile takes it (markdown_core_compile_field) or the run ends. The
 * index of the field's reading, or UINT32_MAX when the reading halts
 * (a field waits on the document as a label does) or failed, `root` then
 * the caller's. */
/* A PASS: the tokenizer and the compiler of one reading -- a run's, a
 * field's inside it, a block start's (blocks.c) -- taken from the parser's
 * finished ones and given back as the reading ends. NULL when storage could
 * not be had. */
typedef struct markdown_core_pass {
    markdown_core_tokenizer tokenizer;
    markdown_core_compiler compiler;
    struct markdown_core_pass *next;
} markdown_core_pass;
markdown_core_pass *markdown_core_pass_take(markdown_core_parser *parser);
void markdown_core_pass_back(markdown_core_parser *parser, markdown_core_pass *pass);

uint32_t markdown_core_text_field(markdown_core_tokenizer *tokenizer, markdown_core_node *root, bool *spaced);
/* The field `index` the run read compiled into its root, which is then the
 * caller's; NULL when it failed. */
markdown_core_node *markdown_core_compile_field(markdown_core_compiler *compiler, uint32_t index);
/* THE COMPILE OF A READING (plan symbols-cleanroom.md 23.5): the run whose
 * nodes compile makes, the node it is filling -- the innermost a handler
 * opened, or the run's owner -- and how a handler places what it makes:
 * appended to that node (its containment asked), or appended and opened,
 * or a Text of the content bytes `from` up to `to`. False, or NULL, when
 * the node could not be had or was refused, which fails the reading. */
static inline markdown_core_inline_state *markdown_core_compile_state(const markdown_core_compiler *compiler) {
    return (markdown_core_inline_state *)compiler->reader;
}
static inline markdown_core_node *markdown_core_compile_parent(const markdown_core_compiler *compiler) {
    markdown_core_node *top = (markdown_core_node *)markdown_core_compile_top(compiler);
    return top ? top : markdown_core_compile_state(compiler)->owner;
}
bool markdown_core_compile_append(markdown_core_compiler *compiler, markdown_core_node *node);
/* THE GIVING UP of a construct (node.h, OPENER, and tokenizer.h, THE
 * NOTES): it opened a construct of kind `opens` whose opening syntax is
 * content bytes `from` up to `to`, and wrote the `count` fields at
 * `fields` as far as it read them. The run's node keeps it once the
 * reading is done. */
enum { MARKDOWN_CORE_GIVE_UP_FIELDS = 4 };
bool markdown_core_text_give_up(markdown_core_tokenizer *tokenizer, markdown_core_node_kind opens, bufsize_t from,
                                bufsize_t to, const markdown_core_field *fields, size_t count);
/* THE NODE A CONSTRUCT MADE THAT A LATER TOKEN MAY UPDATE (no-flush.md
 * 10.13, element.h MARKDOWN_CORE_FOLLOW_TOKEN): the construct just read
 * makes a node of `kind` that a follower of that kind may take what comes
 * right after it for (an attribute block). One made at the content's end
 * read the end: a follower may begin there once the input goes on. The
 * follower asks with markdown_core_text_made_at. */
void markdown_core_text_made(markdown_core_tokenizer *tokenizer, markdown_core_node_type kind);
/* The kind of the node made right before where the reading stands, or
 * NONE. */
static inline markdown_core_node_type markdown_core_text_made_at(const markdown_core_tokenizer *tokenizer) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    return inline_state->made_end == (bufsize_t)tokenizer->position ? inline_state->made_kind : MARKDOWN_CORE_NODE_NONE;
}
bool markdown_core_compile_open(markdown_core_compiler *compiler, markdown_core_node *node);
markdown_core_node *markdown_core_compile_text(markdown_core_compiler *compiler, bufsize_t from, bufsize_t to);

#endif
