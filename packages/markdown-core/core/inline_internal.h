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
#include "houdini.h"
#include "iterator.h"
#include "inlines.h"
#include "element.h"
#include "../elements/markdown-core-elements.h"
#include "../elements/citation_state.h"
#include "../elements/bracket_state.h"

/* One maximal parsed delimiter run, classified from immutable source bytes. The text
 * scanner may retain one lookahead run for delimiter dispatch to consume. */
typedef struct {
    bufsize_t start, end;
    markdown_core_delimiter_rule rule;
    bool can_open, can_close;
} delimiter_run;

struct markdown_core_inline_state {
    markdown_core_chunk input;
    markdown_core_attribute_parser attributes;
    bufsize_t heading_attributes_start, text_end, heading_label_end;
    unsigned flags;
    bufsize_t opaque_end;
    /* Starts before this underscore share a rejected domain suffix. */
    bufsize_t autolink_rejected_until;
    /* One plus the start of a suffix proven to contain no closer of a rule. */
    bufsize_t opaque_failed_from[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int line;
    bufsize_t pos;
    /* The block whose content buffer `input` is, and the parser that holds
     * requirement 10's content-to-source map for it. Both are NULL for a
     * inline state built straight out of a chunk -- the reference-definition
     * parser -- and the map is then simply not consulted. */
    markdown_core_parser *owner_parser;
    markdown_core_node *owner;
    /* `owner`'s structural element, resolved once. The projection is a pure
     * function of `owner->kind`, `owner` does not change across a run, and a
     * run's owner does not change kind during it -- so asking per token was
     * asking the same question once per inline node. Written only beside
     * `owner`, in the one place that assigns it, so the pair cannot drift. */
    const markdown_core_element *owner_structure;
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
     * line, column, step and width. A node whose two ends lie in the frame
     * is placed by arithmetic on these six values and reads nothing from the
     * map; `mapped` is whether there is a map at all -- a state built
     * straight out of a chunk, the reference-definition parser's, has none,
     * and neither has a block that came with no content. */
    bufsize_t mark_run_start, mark_run_end;
    int mark_line, mark_column, mark_step, mark_width;
    bool mapped;
    markdown_core_map *refmap;
    delimiter *last_delim;
    delimiter_run cached_run;
    /* How many delimiters of each rule on the stack can open, and how many
     * can close, kept at every push and removal so a scanner can ask whether
     * a closer will pair without walking the stack. */
    int delim_openers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int delim_closers[MARKDOWN_CORE_DELIM_RULE_COUNT];
    bracket *last_bracket;
    citation_tokens citations;
    citation_brace_index citation_braces;
    bracket *pending_brackets;
    /* One past the last consumed byte other than SP/TAB. This lets every
     * inline-note closer test its body's non-empty rule in constant time. */
    bufsize_t nonblank_end;
    bufsize_t *backticks;
    bufsize_t backtick_capacity;
    bool scanned_for_backticks;
    bool no_link_openers;
    /* The sealed dialect the scan reads its byte tables and delimiter owners
     * from: the owning parser's, or the empty dialect when there is no parser
     * (reference parsing), which registers nothing. */
    const markdown_core_dialect *dialect;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    markdown_core_parse_error error;
};

#define make_str(inline_state, sc, ec, s)                                                                              \
    markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)

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
    const markdown_core_line_mark *mark = &parser->line_marks[cursor];
    inline_state->mark_run_start = mark->content_offset;
    inline_state->mark_run_end = cursor < last ? parser->line_marks[cursor + 1].content_offset : INT32_MAX;
    inline_state->mark_line = mark->line;
    inline_state->mark_column = mark->column;
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
        markdown_core_parser_append_content_mark(inline_state->owner_parser, node, 0, node->start_line,
                                                 node->start_column, node->end_column - node->start_column + 1, 0);
    }
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
 * of its ends are then the frame's line and column plus a distance. Each end
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
    node->start_line = inline_state->mark_line;
    node->end_line = inline_state->mark_line;
    node->start_column =
        inline_state->mark_column + (int)(from_offset - inline_state->mark_run_start) * inline_state->mark_step;
    node->end_column = inline_state->mark_column +
                       (int)(to_offset - inline_state->mark_run_start) * inline_state->mark_step +
                       inline_state->mark_width - 1;
    if (node->kind == MARKDOWN_CORE_NODE_TEXT && node->as.literal->len > 0) {
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
void markdown_core_inline_state_from_buf(markdown_core_parser *parser, int line_number,
                                         markdown_core_inline_state *inline_state, markdown_core_chunk *chunk,
                                         markdown_core_map *refmap);
unsigned char markdown_core_inline_peek_char_n(markdown_core_inline_state *inline_state, bufsize_t n);
unsigned char markdown_core_inline_peek_char(markdown_core_inline_state *inline_state);
unsigned char markdown_core_inline_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos);
int markdown_core_inline_is_eof(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_spaces(markdown_core_inline_state *inline_state);
bool markdown_core_inline_skip_line_end(markdown_core_inline_state *inline_state);
void markdown_core_inline_remove_delimiter(markdown_core_inline_state *inline_state, delimiter *delim);
delimiter *markdown_core_inline_push_delimiter_entry(markdown_core_inline_state *inline_state, delimiter_kind kind,
                                                     bufsize_t position);
void markdown_core_inline_process_delimiters(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                             bufsize_t stack_bottom, delimiter *after);
int markdown_core_inline_parse_inline(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent,
                                        markdown_core_map *refmap, markdown_core_inline_state *inline_state);
void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state);
bool markdown_core_inline_finish_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
markdown_core_node *markdown_core_inline_match_delimiter(const markdown_core_element *element,
                                                         markdown_core_inline_state *inline_state);
int markdown_core_byte_set_has(const char *set, unsigned char character);
void markdown_core_inline_push_boundary(markdown_core_inline_state *inline_state, bufsize_t position);
#endif
