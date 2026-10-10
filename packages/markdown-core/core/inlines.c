#include "alloc.h"
#include "inline_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "markdown_core_ctype.h"
#include "config.h"
#include "node.h"
#include "parser.h"
#include "registry.h"
#include "map.h"
#include "node_type.h"
#include "buffer.h"
#include "houdini.h"
#include "utf8.h"
#include "delimiter.h"
#include "inlines.h"
#include "element.h"

static markdown_core_delimiter_rule delimiter_rule_for_byte(markdown_core_inline_state *inline_state, unsigned char c) {
    return inline_state->dialect->delimiter_chars[c];
}

static delimiter *S_insert_delimited_inline(markdown_core_inline_state *inline_state, delimiter *opener,
                                            delimiter *closer, bufsize_t use_delims, markdown_core_node_type kind);

static bufsize_t inline_state_find_special_char(markdown_core_inline_state *inline_state);

/* The element API's placement: the inline fast path (inline_internal.h),
 * behind a call, for the makers outside this translation unit. */
void markdown_core_inline_state_place(markdown_core_inline_state *inline_state, markdown_core_node *node, int from,
                                      int to) {
    markdown_core_inline_place(inline_state, node, from, to);
}

/* A placement that leaves the frame -- the next line's first token, an
 * emphasis closed across lines, a link placed back at its opener, an end
 * the caller left unresolved -- is answered by the span, which probes from
 * the cursor and moves it to where the node ends; the frame is then read
 * from the run the cursor moved to. The fallback of the old arithmetic is
 * gone: every content-bearing block has a map by the time its inlines are
 * parsed (`markdown_core_inline_start_inlines` gives one to any block whose
 * content was SET rather than fed), and a state with no map places nothing
 * (`mapped`, tested by the fast path before it comes here). */
void markdown_core_inline_place_outside_frame(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                              int from, int to) {
    markdown_core_content_span span;
    markdown_core_parser_content_span(inline_state->owner_parser,
                                      inline_state->owner ? &inline_state->owner->node->content_map : NULL, from, to,
                                      &span, &inline_state->mark_cursor);
    markdown_core_inline_seat_cursor(inline_state);
    if (span.has_start) {
        node->where.place.start = (uint32_t)span.start;
    }
    if (span.has_end) {
        node->where.place.end = (uint32_t)span.end;
    }
    if (node->kind == MARKDOWN_CORE_NODE_TEXT && node->as.literal->len > 0) {
        /* A Text placed with an end unresolved keeps no map at all: the
         * writes stay inside the verbatim gate, and a partial span names no
         * slice to take. */
        if (span.has_start && span.has_end) {
            markdown_core_inline_map_text(inline_state, node, from, to, span.first, span.last);
        } else {
            const markdown_core_chunk *literal = node->as.literal;
            if (!(literal->len == to - from + 1 &&
                  (literal->data == inline_state->input.data + from ||
                   memcmp(literal->data, inline_state->input.data + from, (size_t)literal->len) == 0))) {
                node->content_map.count = 0;
                node->content_map.offset = 0;
                markdown_core_parser_append_content_mark(inline_state->owner_parser, node, 0, inline_state->mark_line,
                                                         node->where.place.start,
                                                         (int)(node->where.place.end - node->where.place.start), 0);
            }
        }
    }
}

// Create an inline with a literal string value.
markdown_core_node *markdown_core_inline_make_literal(markdown_core_inline_state *inline_state,
                                                      markdown_core_node_type t, int start_column, int end_column,
                                                      markdown_core_chunk s) {
    markdown_core_node *e = markdown_core_parser_make_node(inline_state->owner_parser, t);
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(&s);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    *e->as.literal = s;
    markdown_core_inline_place(inline_state, e, start_column, end_column);
    return e;
}

/* Records the kind it creates. Every parse-time caller reaches the parser
 * through its inline state; the mem-only form above stays for callers that
 * have no parse at all. */
markdown_core_node *markdown_core_inline_make_simple(markdown_core_inline_state *inline_state,
                                                     markdown_core_node_type t) {
    return markdown_core_parser_make_node(inline_state->owner_parser, t);
}

/* markdown_core_inline_make_simple with the inline state's loss flag for handlers that consume input
 * before creating the node. */
markdown_core_node *markdown_core_inline_make_simple_with_state(markdown_core_inline_state *inline_state,
                                                                markdown_core_node_type t) {
    markdown_core_node *e = markdown_core_inline_make_simple(inline_state, t);
    if (!e) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return e;
}

/* A run's records, one block of the dialect's run-state size: a
 * block a finished run gave back, or a new one, zeroed either way. A dialect
 * whose elements own no inline state gives a run no block. */
typedef struct markdown_core_inline_record {
    struct markdown_core_inline_record *next;
} markdown_core_inline_record;

static unsigned char *S_take_inline_records(markdown_core_parser *parser, size_t size) {
    markdown_core_inline_record *record = parser->free_inline_records;
    if (!record) {
        return markdown_core_alloc(1, size);
    }
    parser->free_inline_records = record->next;
    memset(record, 0, size);
    return (unsigned char *)record;
}

static void S_give_inline_records(markdown_core_parser *parser, unsigned char *records) {
    markdown_core_inline_record *record = (markdown_core_inline_record *)(void *)records;
    record->next = parser->free_inline_records;
    parser->free_inline_records = record;
}

void markdown_core_inline_release_records(markdown_core_parser *parser) {
    while (parser->free_inline_records) {
        markdown_core_inline_record *record = parser->free_inline_records;
        parser->free_inline_records = record->next;
        markdown_core_free(record);
    }
}

/* Whether the run began: a run whose records could not be allocated never
 * calls a lifecycle hook, since every one of them reads its records. */
static bool S_inline_run_began(const markdown_core_inline_state *inline_state) {
    return inline_state->run_state || !inline_state->dialect->run_state_size;
}

void markdown_core_inline_state_from_buf(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         markdown_core_chunk *chunk) {
    memset(inline_state, 0, sizeof(*inline_state));
    inline_state->input = *chunk;
    inline_state->owner_parser = parser;
    inline_state->text_end = -1;
    inline_state->now = INT32_MAX;
    if (parser) {
        inline_state->dialect = parser->dialect;
        size_t records = parser->dialect->run_state_size;
        if (records) {
            inline_state->run_state = S_take_inline_records(parser, records);
            if (!inline_state->run_state) {
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                return;
            }
        }
        const markdown_core_element_instance *const *owners =
            parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_INIT];
        size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_INIT];
        for (size_t i = 0; i < count; i++) {
            parser->inline_hook_work++;
            owners[i]->element->init_inline(owners[i], inline_state);
        }
    }
}

unsigned char markdown_core_inline_peek_char_n(markdown_core_inline_state *inline_state, bufsize_t n) {
    // NULL bytes should have been stripped out by now.  If they're
    // present, it's a programming error:
    assert(!(inline_state->pos + n < inline_state->input.len && inline_state->input.data[inline_state->pos + n] == 0));
    return (inline_state->pos + n < inline_state->input.len) ? inline_state->input.data[inline_state->pos + n] : 0;
}

unsigned char markdown_core_inline_peek_char(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_peek_char_n(inline_state, 0);
}

unsigned char markdown_core_inline_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos) {
    return inline_state->input.data[pos];
}

// Reads the flanking skip table for the byte at `pos`, which must be inside the
// chunk.
//
// The flanking scan used to subscript this table with the byte at `pos` BEFORE
// testing that `pos` was in range. It got away with it because
// `markdown_core_parse_inlines` builds its chunk from a `markdown_core_strbuf`,
// and a strbuf always keeps `ptr[size] == '\0'` inside its own allocation --
// an invariant stated in buffer.c and nowhere near here. Any chunk that is a
// slice of a larger buffer makes the read live, silently: no sanitizer can see
// it, measured, with 0 ASan reports over 14,783 executions of it. The assertion
// is what keeps the operand order from drifting back.
static MARKDOWN_CORE_INLINE unsigned char flanking_skip_at(markdown_core_inline_state *inline_state, bufsize_t pos) {
    assert(pos < inline_state->input.len);
    return inline_state->dialect->skip_chars[markdown_core_inline_peek_at(inline_state, pos)];
}

// Return true if there are more characters in the inline state.
int markdown_core_inline_is_eof(markdown_core_inline_state *inline_state) {
    return (inline_state->pos >= inline_state->input.len);
}

// Advance the inline state.  Doesn't check for eof.
#define advance(inline_state) (inline_state)->pos += 1

bool markdown_core_inline_skip_spaces(markdown_core_inline_state *inline_state) {
    bool skipped = false;
    while (markdown_core_is_space_or_tab(markdown_core_inline_peek_char(inline_state))) {
        advance(inline_state);
        skipped = true;
    }
    return skipped;
}

bool markdown_core_inline_skip_line_end(markdown_core_inline_state *inline_state) {
    bool seen_line_end_char = false;
    if (markdown_core_inline_peek_char(inline_state) == '\r') {
        advance(inline_state);
        seen_line_end_char = true;
    }
    if (markdown_core_inline_peek_char(inline_state) == '\n') {
        advance(inline_state);
        seen_line_end_char = true;
    }
    return seen_line_end_char || markdown_core_inline_is_eof(inline_state);
}

// Take characters while a predicate holds, and return a string.
static const delimiter_rule_spec *delimiter_spec(markdown_core_inline_state *inline_state,
                                                 markdown_core_delimiter_rule rule) {
    const markdown_core_element_instance *owner = inline_state->dialect->delimiter_owners[rule];
    static const delimiter_rule_spec empty = {0};
    return owner ? &owner->element->delimiter : &empty;
}

/* Classify without moving the parser cursor or allocating an AST node.
 * A cached lookahead is keyed by its source offset, so text scanning and
 * delimiter dispatch consume the same classification even after a rewind. */
static const delimiter_run *S_scanned(markdown_core_inline_state *inline_state, const delimiter_run *run) {
    inline_state->cached_run = *run;
    markdown_core_inline_state_read(inline_state, run->low, run->reach);
    return &inline_state->cached_run;
}

static const delimiter_run *scan_delimiter(markdown_core_inline_state *inline_state, bufsize_t start,
                                           markdown_core_delimiter_rule rule) {
    if (inline_state->cached_run.rule != MARKDOWN_CORE_DELIM_RULE_NONE && inline_state->cached_run.start == start &&
        inline_state->cached_run.rule == rule) {
        return S_scanned(inline_state, &inline_state->cached_run);
    }
    unsigned char c = markdown_core_inline_peek_at(inline_state, start);
    delimiter_run run = {.start = start, .end = start, .rule = rule};
    assert(run.rule != MARKDOWN_CORE_DELIM_RULE_NONE);
    const delimiter_rule_spec *spec = delimiter_spec(inline_state, run.rule);
    while (run.end < inline_state->input.len && markdown_core_inline_peek_at(inline_state, run.end) == c &&
           (!spec->run_limit || run.end - run.start < spec->run_limit)) {
        run.end++;
        if (inline_state->owner_parser) {
            inline_state->owner_parser->delimiter_work++;
        }
    }
    /* The run read its bytes and the byte that ended it. */
    run.low = run.start;
    run.reach = run.end + 1;
    if (spec->body == DELIMITER_WORD_BODY) {
        run.can_open = run.can_close = true;
        return S_scanned(inline_state, &run);
    }
    if (run.end - run.start < spec->minimum_width || (spec->exact_run && run.end - run.start != spec->minimum_width)) {
        return S_scanned(inline_state, &run);
    }

    /* The start and the end of the input read as a newline, and so does a run
     * of skip characters that reaches either. Skip characters are ASCII (the
     * dialect refuses an element that declares any other), so a decoded
     * scalar below 0x80 is its own byte. */
    int32_t before_char = 10, after_char = 10;
    run.low = -1;
    if (run.start > 0) {
        bufsize_t before_char_pos = run.start - 1;
        /* Walk back over skip characters and then over the continuation bytes
         * of the character before them. In UTF-8 a continuation byte follows
         * only a lead or another continuation byte, never a skip character,
         * so the walk also stops at a skip character that a continuation byte
         * follows. Without that stop, stray continuation bytes between `~~`
         * runs let every run walk back across the ones before it, to the
         * start of its paragraph. */
        while ((markdown_core_inline_peek_at(inline_state, before_char_pos) >> 6 == 2 ||
                (inline_state->dialect->skip_chars[markdown_core_inline_peek_at(inline_state, before_char_pos)] &&
                 markdown_core_inline_peek_at(inline_state, before_char_pos + 1) >> 6 != 2)) &&
               before_char_pos > 0) {
            before_char_pos--;
        }
        markdown_core_utf8proc_decode(inline_state->input.data + before_char_pos, run.start - before_char_pos,
                                      &before_char);
        /* A walk that reached the first byte read where the content starts. */
        run.low = before_char_pos > 0 ? before_char_pos : -1;
        if (before_char < 0x80 && inline_state->dialect->skip_chars[before_char]) {
            before_char = 10;
        }
    }
    bufsize_t after_char_pos = run.end;
    while (after_char_pos < inline_state->input.len && flanking_skip_at(inline_state, after_char_pos)) {
        after_char_pos++;
    }
    if (after_char_pos < inline_state->input.len) {
        markdown_core_utf8proc_decode(inline_state->input.data + after_char_pos,
                                      inline_state->input.len - after_char_pos, &after_char);
    }
    /* A character is at most four bytes, and one cut short by the end of
     * the content read that end. */
    run.reach = after_char_pos + 4;
    const uint8_t before = markdown_core_utf8proc_classes(before_char);
    const uint8_t after = markdown_core_utf8proc_classes(after_char);
    const bool space_before = before & MARKDOWN_CORE_UNICODE_WHITESPACE;
    const bool space_after = after & MARKDOWN_CORE_UNICODE_WHITESPACE;
    const bool punct_before = before & MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL;
    const bool punct_after = after & MARKDOWN_CORE_UNICODE_PUNCTUATION_OR_SYMBOL;
    bool left_flanking = !space_after && (!punct_after || space_before || punct_before);
    bool right_flanking = !space_before && (!punct_before || space_after || punct_after);
    if (spec->punctuation_bound) {
        run.can_open = left_flanking && (!right_flanking || punct_before);
        run.can_close = right_flanking && (!left_flanking || punct_after);
    } else {
        run.can_open = left_flanking;
        run.can_close = right_flanking;
    }
    return S_scanned(inline_state, &run);
}

/* Source classification is immutable, but eligibility depends on the live
 * stack. A close-only run cannot match a future opener. Keep it as text when
 * no earlier opener of its rule survives; runs that can open must remain
 * eligible even without an earlier opener. Counts are conservative because
 * pair reduction is deferred and one run can supply several delimiter units. */
static bool delimiter_needs_stack(markdown_core_inline_state *inline_state, const delimiter_run *run) {
    if (run->can_open || !run->can_close) {
        return run->can_open;
    }
    markdown_core_inline_read_rule(inline_state, run->rule);
    return inline_state->delim_openers[run->rule] > 0;
}

/*
static void print_delimiters(markdown_core_inline_state *inline_state)
{
        delimiter *delim;
        delim = inline_state->last_delim;
        while (delim != NULL) {
                printf("Item at stack pos %p: %d %d %d next(%p) prev(%p)\n",
                       (void*)delim, (int)delim->rule,
                       delim->can_open, delim->can_close,
                       (void*)delim->next, (void*)delim->previous);
                delim = delim->previous;
        }
}
*/

void markdown_core_inline_remove_delimiter(markdown_core_inline_state *inline_state, delimiter *delim) {
    if (delim == NULL) {
        return;
    }
    if (delim->next == NULL) {
        // end of list:
        assert(delim == inline_state->last_delim);
        inline_state->last_delim = delim->previous;
    } else {
        delim->next->previous = delim->previous;
    }
    if (delim->previous != NULL) {
        delim->previous->next = delim->next;
    }
    if (delim->can_open) {
        inline_state->delim_openers[delim->rule]--;
    }
    if (delim->can_close) {
        inline_state->delim_closers[delim->rule]--;
    }
    if (!inline_state->delim_openers[delim->rule] && !inline_state->delim_closers[delim->rule]) {
        inline_state->delim_rules &= ~(1u << delim->rule);
    }
    if (delim->stay) {
        inline_state->owner_parser->stays[delim->stay - 1] = inline_state->now;
    }
    /* Returned to the parser's pool, not to the allocator: see the push. */
    delim->next = inline_state->owner_parser->free_delimiters;
    inline_state->owner_parser->free_delimiters = delim;
}

/* DELIMITER ENTRIES ARE POOLED BY THE PARSER. An entry lives from its push to
 * the reduction or clearing that removes it, which is within one inline
 * container's parse, and a document pushes one per marker run and one per
 * whitespace boundary between them. Taking each from the allocator and giving
 * it back was the largest fixed cost per delimiter in an emphasis-heavy
 * document, larger than classifying the run. The parser keeps the entries it
 * has removed on a free list and hands them out again, so the allocator is
 * asked only when more entries are live at once than ever were before: the
 * pool's size is the largest live count in the document, and it is released
 * with the parser. Every inline state that pushes has a parser; the one built
 * without (link.c, `S_reference_definition`) scans a label and pushes
 * nothing. */
delimiter *markdown_core_inline_push_delimiter_entry(markdown_core_inline_state *inline_state, delimiter_kind kind,
                                                     bufsize_t position) {
    markdown_core_parser *parser = inline_state->owner_parser;
    assert(parser);
    parser->delimiter_pushes++;
    delimiter *entry = parser->free_delimiters;
    if (entry) {
        parser->free_delimiters = entry->next;
        memset(entry, 0, sizeof(*entry));
    } else {
        entry = (delimiter *)markdown_core_alloc(1, sizeof(delimiter));
        if (!entry) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            return NULL;
        }
    }
    entry->kind = kind;
    entry->position = position;
    entry->previous = inline_state->last_delim;
    if (entry->previous) {
        entry->previous->next = entry;
    }
    inline_state->last_delim = entry;
    return entry;
}

void markdown_core_inline_push_boundary(markdown_core_inline_state *inline_state, bufsize_t position) {
    inline_state->token.flags |= MARKDOWN_CORE_INLINE_BOUNDARY;
    if (inline_state->last_delim && inline_state->last_delim->kind == DELIMITER_BOUNDARY) {
        inline_state->last_delim->position = position;
    } else {
        markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_BOUNDARY, position);
    }
}

/* Reduce a completed range to its last content boundary. Markers cannot
 * escape a completed container, but its whitespace still constrains an
 * enclosing word body. Keeping one summary also bounds repeated work across
 * nested bracket scopes: each removed entry is visited only once. */
static void reduce_delimiter_range(markdown_core_inline_state *inline_state, delimiter *before, delimiter *after) {
    delimiter *entry = after ? after->previous : inline_state->last_delim;
    bool boundary = false;
    while (entry != before) {
        delimiter *previous = entry->previous;
        assert(entry->kind != DELIMITER_FIELD);
        if (inline_state->owner_parser) {
            inline_state->owner_parser->delimiter_work++;
        }
        if (entry->kind == DELIMITER_BOUNDARY && !boundary) {
            boundary = true;
        } else {
            markdown_core_inline_remove_delimiter(inline_state, entry);
        }
        entry = previous;
    }
}

/* A token's owned fields finish before scanning its successor, preserving
 * reference occurrence order. Heading declaration can suspend with this
 * event on the same stack and resume after its symbol table is complete. */
static void complete_inline_token(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    delimiter *entry = inline_state->last_delim;
    if (!entry || entry->kind != DELIMITER_FIELD) {
        return;
    }
    bool whitespace = markdown_core_parse_inline_subtrees(parser, entry->member);
    if (whitespace) {
        entry->kind = DELIMITER_BOUNDARY;
        entry->member = NULL;
        if (entry->previous && entry->previous->kind == DELIMITER_BOUNDARY) {
            markdown_core_inline_remove_delimiter(inline_state, entry->previous);
        }
    } else {
        markdown_core_inline_remove_delimiter(inline_state, entry);
    }
}

/* THE MARKER'S STAY on the stack begins, for the token that made `member`:
 * it lasts until a reduction removes the marker (`now`). A marker pushed for
 * a member another token made leaves this token unrecorded. */
static void S_stay(markdown_core_inline_state *inline_state, delimiter *delim, markdown_core_member *member) {
    markdown_core_parser *parser = inline_state->owner_parser;
    int32_t *stays =
        markdown_core_reserve(parser->stays, &parser->stay_capacity, parser->stay_count + 1, sizeof(*stays));
    if (!stays) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    parser->stays = stays;
    stays[parser->stay_count++] = INT32_MAX;
    delim->stay = (uint32_t)parser->stay_count;
    if (member == inline_state->token_member) {
        inline_state->token.stay = delim->stay;
    } else {
        markdown_core_inline_unrecorded(inline_state);
    }
}

static void push_delimiter(markdown_core_inline_state *inline_state, const markdown_core_element_instance *owner,
                           markdown_core_delimiter_rule rule, bool can_open, bool can_close,
                           markdown_core_member *inl_text) {
    delimiter *delim;
    /* Elements may pass NULL after their own allocation failures. */
    if (!inl_text) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    /* `openers_bottom` is sized by MARKDOWN_CORE_DELIM_RULE_COUNT, so a rule
     * outside the enum is an out-of-bounds index. The parameter is typed, but
     * the push is public and C will convert anything to an enum, so the bound
     * is enforced rather than assumed: an unnamed rule is not a delimiter, and
     * the literal text node stays in the tree as ordinary text. */
    if (rule <= MARKDOWN_CORE_DELIM_RULE_NONE || rule >= MARKDOWN_CORE_DELIM_RULE_COUNT) {
        return;
    }
    delim = markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_MARKER, inline_state->pos);
    if (!delim) {
        return;
    }
    delim->owner = owner;
    delim->rule = rule;
    delim->can_open = can_open;
    delim->can_close = can_close;
    delim->member = inl_text;
    delim->length = inl_text->node->as.literal->len;
    if (can_open) {
        inline_state->delim_openers[rule]++;
    }
    if (can_close) {
        inline_state->delim_closers[rule]++;
    }
    if (can_open || can_close) {
        inline_state->delim_rules |= 1u << rule;
    }
    if (inline_state->records) {
        S_stay(inline_state, delim, inl_text);
    }
}

static markdown_core_member *handle_delim(markdown_core_inline_state *inline_state, const delimiter_run *run) {
    assert(delimiter_needs_stack(inline_state, run));
    inline_state->pos = run->end;
    markdown_core_member *inl_text = markdown_core_inline_state_append(
        inline_state, make_str(inline_state, run->start, run->end - 1,
                               markdown_core_chunk_dup(&inline_state->input, run->start, run->end - run->start)));
    // One eligible maximal run owns one stack entry and cannot match itself.
    if (inl_text) {
        push_delimiter(inline_state, inline_state->dialect->delimiter_owners[run->rule], run->rule, run->can_open,
                       run->can_close, inl_text);
    }
    return inl_text;
}

int markdown_core_byte_set_has(const char *set, unsigned char c) {
    const unsigned char *p;

    if (set == NULL || c == 0) {
        return 0;
    }
    for (p = (const unsigned char *)set; *p; p++) {
        if (*p == c) {
            return 1;
        }
    }
    return 0;
}

void markdown_core_inline_process_delimiters(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                             bufsize_t stack_bottom, delimiter *after) {
    delimiter *candidate;
    delimiter *closer = after;
    delimiter *opener;
    delimiter *old_closer;
    bool opener_found;
    /* One slot per RULE, so the array is sized by construction. It used to be
     * `[3][128]` indexed by a byte the public push accepts unconstrained. */
    bufsize_t openers_bottom[3][MARKDOWN_CORE_DELIM_RULE_COUNT];
    bufsize_t word_bottom = stack_bottom;
    bufsize_t affix_bottom = stack_bottom;
    int i;

    // initialize openers_bottom:
    for (i = 0; i < 3; i++) {
        for (int rule = 0; rule < MARKDOWN_CORE_DELIM_RULE_COUNT; rule++) {
            openers_bottom[i][rule] = stack_bottom;
        }
    }

    // move back to first relevant delim.
    candidate = after ? after->previous : inline_state->last_delim;
    while (candidate != NULL && candidate->position >= stack_bottom) {
        closer = candidate;
        candidate = candidate->previous;
    }

    // now move forward, looking for closers, and handling each
    //
    // EVERY ARM ADVANCES `closer`, and that is D33. The old chain was
    // `if (element) ... else if (delim_char is * or _) ... else if (' or ")`,
    // so a delimiter that matched none of the three -- which is what a byte
    // whose owner cannot be found looks like -- left `closer` where it was,
    // fell into the removal below, freed it, and read it again on the next
    // turn. With `can_open` set, nothing freed it and the loop never ended.
    // The quote arm is gone with smart punctuation: a quotation mark is
    // ordinary text and pushes no delimiter.
    while (closer != after) {
        const markdown_core_element_instance *owner = closer->owner;
        inline_state->now = closer->position;
        /* Positions are ordered along this range, including retained citation
         * tokens and completed-field boundaries. A boundary therefore dominates
         * every earlier failed search. Store its shared cause once, not once per
         * rule/residue; the effective bound is their maximum at the query. */
        if (closer->kind == DELIMITER_BOUNDARY) {
            assert(closer->position >= word_bottom);
            word_bottom = closer->position;
        } else if (closer->kind == DELIMITER_AFFIX_BOUNDARY) {
            assert(closer->position >= affix_bottom);
            affix_bottom = closer->position;
        }
        assert(closer->kind != DELIMITER_FIELD);
        if (closer->can_close) {
            // Now look backwards for first matching opener:
            opener = closer->previous;
            opener_found = false;
            bufsize_t bottom = openers_bottom[closer->length % 3][closer->rule];
            if (bottom < affix_bottom) {
                bottom = affix_bottom;
            }
            if (delimiter_spec(inline_state, closer->rule)->body == DELIMITER_WORD_BODY && bottom < word_bottom) {
                bottom = word_bottom;
            }
            while (opener != NULL && opener->position >= bottom) {
                if (inline_state->owner_parser) {
                    inline_state->owner_parser->delimiter_work++;
                }
                if (opener->can_open && opener->rule == closer->rule) {
                    // interior closer of size 2 can't match opener of size 1
                    // or of size 1 can't match 2
                    if (!delimiter_spec(inline_state, closer->rule)->rule_of_three ||
                        !(closer->can_open || opener->can_close) || closer->length % 3 == 0 ||
                        (opener->length + closer->length) % 3 != 0) {
                        opener_found = true;
                        break;
                    }
                }
                opener = opener->previous;
            }
            old_closer = closer;

            if (opener_found) {
                reduce_delimiter_range(inline_state, opener, closer);
                const delimiter_rule_spec *spec = delimiter_spec(inline_state, closer->rule);
                if (spec->minimum_width) {
                    bufsize_t used = spec->maximum_width;
                    if (opener->member->node->as.literal->len < used || closer->member->node->as.literal->len < used) {
                        used = spec->minimum_width;
                    }
                    markdown_core_node_type kind = used == 2 ? spec->double_kind : spec->single_kind;
                    closer = S_insert_delimited_inline(inline_state, opener, closer, used, kind);
                } else if (owner && owner->element->insert_inline_from_delim) {
                    delimiter *next = closer->next;
                    owner->element->insert_inline_from_delim(owner, parser, inline_state, opener, closer);
                    markdown_core_inline_remove_delimiter(inline_state, opener);
                    markdown_core_inline_remove_delimiter(inline_state, closer);
                    closer = next;
                } else {
                    closer = closer->next;
                }
            } else {
                closer = closer->next;
            }
            if (!opener_found) {
                /* The closer searched the stack of its rule in vain. */
                old_closer->member->reads.rules |= 1u << old_closer->rule;
                // set lower bound for future searches for openers
                openers_bottom[old_closer->length % 3][old_closer->rule] = old_closer->position;
                if (!old_closer->can_open) {
                    // we can remove a closer that can't be an
                    // opener, once we've seen there's no
                    // matching opener:
                    markdown_core_inline_remove_delimiter(inline_state, old_closer);
                }
            }
        } else {
            closer = closer->next;
        }
    }
    inline_state->now = after ? after->position : INT32_MAX;
    reduce_delimiter_range(inline_state, candidate, after);
    inline_state->now = INT32_MAX;
}

/* WHAT A PAIR OF RUNS READ, for the inline they make of `used` bytes of each
 * (5.6): the opener's token, which begins the inline when it gives all its
 * bytes, and the closer's rules and reach; the inline ends `used` bytes into
 * the closer. A run with bytes left is a Text the pair cut, which no later
 * parse takes whole, and the closer's bytes left begin past the ones used. */
static void S_read_delimited(markdown_core_member *inline_member, markdown_core_member *opener_member,
                             markdown_core_member *closer_member, bufsize_t opener_left, bufsize_t closer_left,
                             bufsize_t used) {
    markdown_core_inline_reads *reads = &inline_member->reads;
    const markdown_core_inline_reads *closing = &closer_member->reads;
    *reads = opener_member->reads;
    reads->start += opener_left;
    reads->end = closing->start + used;
    reads->rules |= closing->rules;
    reads->reach = reads->reach > closing->reach ? reads->reach : closing->reach;
    reads->flags &= closing->flags | ~(uint32_t)(MARKDOWN_CORE_INLINE_RECORDED | MARKDOWN_CORE_INLINE_LOCAL);
    reads->flags |= closing->flags & MARKDOWN_CORE_INLINE_CONTEXT;
    if (opener_left > 0) {
        reads->flags &= ~(uint32_t)MARKDOWN_CORE_INLINE_RECORDED;
        opener_member->reads.flags &= ~(uint32_t)MARKDOWN_CORE_INLINE_RECORDED;
        opener_member->reads.end -= used;
    }
    if (closer_left > 0) {
        closer_member->reads.flags &= ~(uint32_t)MARKDOWN_CORE_INLINE_RECORDED;
        closer_member->reads.start += used;
    }
}

static delimiter *S_insert_delimited_inline(markdown_core_inline_state *inline_state, delimiter *opener,
                                            delimiter *closer, bufsize_t use_delims, markdown_core_node_type kind) {
    delimiter *tmp_delim;
    markdown_core_member *opener_member = opener->member;
    markdown_core_member *closer_member = closer->member;
    markdown_core_node *opener_inl = opener_member->node;
    markdown_core_node *closer_inl = closer_member->node;
    bufsize_t opener_num_chars = opener_inl->as.literal->len;
    bufsize_t closer_num_chars = closer_inl->as.literal->len;
    markdown_core_node *inline_node;
    const bufsize_t minimum_width = delimiter_spec(inline_state, closer->rule)->minimum_width;

    /* A rejected container leaves its authored text intact for every rule. */
    if (!markdown_core_node_can_contain_type(opener_member->owner->node, kind)) {
        /* Both runs stay text because of the node that holds them. */
        opener_member->reads.flags |= MARKDOWN_CORE_INLINE_CONTEXT;
        closer_member->reads.flags |= MARKDOWN_CORE_INLINE_CONTEXT;
        delimiter *next = closer->next;
        markdown_core_inline_remove_delimiter(inline_state, opener);
        markdown_core_inline_remove_delimiter(inline_state, closer);
        return next;
    }

    // Allocate before mutating either run. OOM leaves the source intact and
    // aborts the shared parse transaction.
    inline_node = markdown_core_inline_make_simple(inline_state, kind);
    markdown_core_member *inline_member =
        inline_node ? markdown_core_parser_member(inline_state->owner_parser, inline_node, true) : NULL;
    if (!inline_member) {
        if (inline_node) {
            markdown_core_parser_release_node(inline_state->owner_parser, inline_node);
        }
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return closer->next;
    }

    inline_node->element = closer->owner ? closer->owner->element : NULL;

    // remove used characters from associated inlines.
    opener_num_chars -= use_delims;
    closer_num_chars -= use_delims;
    if (inline_state->records) {
        S_read_delimited(inline_member, opener_member, closer_member, opener_num_chars, closer_num_chars, use_delims);
    }
    opener_inl->as.literal->len = opener_num_chars;
    closer_inl->as.literal->len = closer_num_chars;

    /* The members between the two runs become the inline's children, and
     * the inline takes their place. */
    markdown_core_member *first = opener_member->next;
    if (first != closer_member) {
        markdown_core_member *last = closer_member->prev;
        for (markdown_core_member *moved = first;; moved = moved->next) {
            if (inline_state->owner_parser) {
                inline_state->owner_parser->delimiter_work++;
            }
            moved->owner = inline_member;
            if (moved == last) {
                break;
            }
        }
        inline_member->first = first;
        inline_member->last = last;
        first->prev = NULL;
        last->next = NULL;
    }
    opener_member->next = inline_member;
    closer_member->prev = inline_member;
    inline_member->prev = opener_member;
    inline_member->next = closer_member;
    inline_member->owner = opener_member->owner;
    markdown_core_member_inherit(opener_member->owner, inline_member);

    /* REQUIREMENT 11b: the delimiters the inline USED are now its markers.
     * They were claimed CONTENT when they were read, because a `*` that matches
     * nothing is its own literal; this claim is later and wins. `position` is
     * the content offset one past the run, which is why the opener's used bytes
     * are counted back from it and the closer's forward from its own start. */
    // The inline takes the delimiters ADJACENT TO ITS CONTENT -- the opener's
    // trailing `use_delims` and the closer's leading ones -- so what is left over
    // is the opener's LEADING bytes and the closer's TRAILING ones. Taking the
    // whole run's start and end gave two nodes one byte: `***a**` reported a
    // leftover Text spanning columns 1..3 and a Strong also starting at 1.
    inline_node->where.place.start = opener_inl->where.place.start + (uint32_t)opener_num_chars;
    inline_node->where.place.end = closer_inl->where.place.end - (uint32_t)closer_num_chars;
    // and a leftover that SURVIVES owns only the bytes it still carries. A
    // leftover with none is freed below, and writing its end first would put a
    // reversed range in the tree for the length of two statements -- true only
    // by reading ahead, which is not a property worth relying on.
    if (opener_num_chars > 0) {
        opener_inl->where.place.end = opener_inl->where.place.start + (uint32_t)opener_num_chars;
    }
    if (closer_num_chars > 0) {
        closer_inl->content_map.offset += (int)use_delims;
        closer_inl->where.place.start = closer_inl->where.place.end - (uint32_t)closer_num_chars;
    }

    // if opener has 0 characters, remove it and its associated inline
    if (opener_num_chars == 0) {
        markdown_core_parser_release_member(inline_state->owner_parser, opener_member);
        markdown_core_inline_remove_delimiter(inline_state, opener);
    } else if (opener_num_chars < minimum_width) {
        markdown_core_inline_remove_delimiter(inline_state, opener); // A remaining single sign is only text.
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
        // remove empty closer inline
        markdown_core_parser_release_member(inline_state->owner_parser, closer_member);
        // remove closer from list
        tmp_delim = closer->next;
        markdown_core_inline_remove_delimiter(inline_state, closer);
        closer = tmp_delim;
    } else if (closer_num_chars < minimum_width) {
        tmp_delim = closer->next;
        markdown_core_inline_remove_delimiter(inline_state, closer);
        closer = tmp_delim;
    }

    return closer;
}

/* Asks `owner` whether the token being read stops at `at`; a token whose
 * owner did not say what it read is not recorded. */
static bool S_is_inline_start(const markdown_core_element_instance *owner, markdown_core_inline_state *inline_state,
                              bufsize_t at) {
    inline_state->declared = false;
    bool start = owner->element->is_inline_start(owner, inline_state, at);
    if (!inline_state->declared) {
        markdown_core_inline_outside(inline_state);
    }
    return start;
}

static bufsize_t inline_state_find_special_char(markdown_core_inline_state *inline_state) {
    // The caller has already established that the first byte is literal.
    // The dialect is sealed, so its tables are read through one local.
    const markdown_core_dialect *const dialect = inline_state->dialect;
    const unsigned char *const data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    bufsize_t n = inline_state->pos;
    while (n < len) {
        /* Text runs to the next byte an inline element terminates it at. */
        n = markdown_core_scan_to_class(dialect->special_chars, MARKDOWN_CORE_TEXT_END, data, n, len);
        if (n >= len) {
            break;
        }
        unsigned char c = data[n];
        const markdown_core_element_instance *start_owner = dialect->inline_start_owners[c];
        if (start_owner && !S_is_inline_start(start_owner, inline_state, n)) {
            n++;
        } else if (delimiter_rule_for_byte(inline_state, c) != MARKDOWN_CORE_DELIM_RULE_NONE) {
            const delimiter_run *run = scan_delimiter(inline_state, n, delimiter_rule_for_byte(inline_state, c));
            if (delimiter_needs_stack(inline_state, run)) {
                assert(n > inline_state->pos);
                return n;
            }
            // A run that cannot delimit belongs to the current text slice.
            n = run->end;
        } else if (n > inline_state->pos) {
            return n;
        } else {
            n++;
        }
    }
    return inline_state->input.len;
}

static markdown_core_member *try_elements(markdown_core_parser *parser, markdown_core_member *parent, unsigned char c,
                                          markdown_core_inline_state *inline_state) {
    markdown_core_member *res = NULL;
    bufsize_t start = inline_state->pos;
    /* The dialect is sealed, so the byte's owner list is read once. */
    const markdown_core_dialect *dialect = parser->dialect;
    const markdown_core_element_instance *const *owner = dialect->inline_dispatch + dialect->inline_dispatch_offsets[c];
    const markdown_core_element_instance *const *end =
        dialect->inline_dispatch + dialect->inline_dispatch_offsets[c + 1];

    for (; owner < end; owner++) {
        inline_state->declared = false;
        res = (*owner)->element->match_inline(*owner, parser, parent, c, inline_state);
        if (!inline_state->declared) {
            markdown_core_inline_outside(inline_state);
        }

        if (res || inline_state->pos != start || parser->error || inline_state->error) {
            break;
        }
    }

    return res;
}

typedef struct {
    markdown_core_parser *parser;
    markdown_core_member *token;
} field_context;

static int S_attach_token_field(markdown_core_node **slot, void *context) {
    field_context *fields = context;
    return !*slot || markdown_core_parser_attach_field(fields->parser, fields->token, *slot) != NULL;
}

/* A TOKEN JOINS THE CONTENT BEING BUILT: its member holds it, each field root
 * it holds is built with it, and a token with fields waits on the stack for
 * their parse, which comes before the next token is read
 * (complete_inline_token). */
markdown_core_member *markdown_core_inline_state_append(markdown_core_inline_state *inline_state,
                                                        markdown_core_node *token) {
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_member *owner = inline_state->owner;
    if (!token) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    /* Fixed inline owners admit every token their grammar constructs. Only a
     * dynamic policy needs a decision here; it may change during a hook. */
    if (owner->node->element && owner->node->element->can_contain_func &&
        !markdown_core_node_can_contain_type(owner->node, (markdown_core_node_type)token->kind)) {
        /* A token constructor has consumed input. Refusing its result is a
         * failed parse, but the detached token is still ours to release. */
        markdown_core_parser_release_node(parser, token);
        if (!inline_state->error) {
            inline_state->error = MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED;
        }
        return NULL;
    }
    markdown_core_member *member = markdown_core_parser_attach(parser, owner, token, NULL);
    if (!member) {
        return NULL;
    }
    /* A token is taken whole as one node: one that appends more is read
     * again by every later parse. */
    if (inline_state->token_member) {
        markdown_core_inline_outside(inline_state);
    } else if (inline_state->records) {
        inline_state->token_member = member;
    }
    field_context context = {parser, member};
    if (!markdown_core_node_visit_fields(token, S_attach_token_field, &context)) {
        return NULL;
    }
    if (member->fields) {
        delimiter *entry = markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_FIELD, inline_state->pos);
        if (entry) {
            entry->member = member;
        }
    }
    return member;
}

/* WHAT THE STACK HOLDS WHERE A TOKEN BEGINS (node.h,
 * markdown_core_inline_reads): the rules with entries on it, and HELD when
 * an opaque body runs on past here or an element keeps a token open. */
static uint32_t S_stack_state(markdown_core_inline_state *inline_state) {
    const uint32_t state = inline_state->delim_rules;
    if (inline_state->pos < inline_state->opaque_end) {
        return state | MARKDOWN_CORE_INLINE_HELD;
    }
    const markdown_core_dialect *dialect = inline_state->dialect;
    const markdown_core_element_instance *const *owners = dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_HOLDS];
    for (size_t i = 0; i < dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_HOLDS]; i++) {
        if (owners[i]->element->holds_inline(owners[i], inline_state)) {
            return state | MARKDOWN_CORE_INLINE_HELD;
        }
    }
    return state;
}

/* THE CURSOR OVER THE OLD CONTENT (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.6). A root whose content is read again holds the old content of the root
 * it continues, and each token the parse is about to read asks the cursor for
 * the old node that begins where the token would: the parse takes it whole
 * when the bytes its decisions read are the same bytes, read from the same
 * source, and the stack holds what its entry says. The cursor reads the old
 * nodes in order, as a stack of levels: each level is the old nodes of one
 * stem, the next one's index, and the old content offset where the one before
 * it ends.
 *
 * The two contents are mapped through their pieces (node.h,
 * markdown_core_runs_pieces): each piece is a stretch of source one decoding
 * reads, at the content offset its bytes begin. A piece that decodes as many
 * bytes as it reads copies them. An old content byte continues when a copy
 * piece read it from a source byte no edit touched, and the byte its now_source
 * gives is read by a copy piece of the new content. */
typedef struct {
    uint32_t source, span, decoded, content;
} inline_piece;

typedef struct {
    const markdown_core_stem *stem;
    size_t next, count;
    int32_t at;
} inline_level;

struct markdown_core_inline_cursor {
    inline_piece *old_pieces, *new_pieces;
    size_t old_count, new_count;
    int32_t old_size, new_size;
    inline_level *levels;
    size_t depth, capacity;
    /* Every content byte is a copy of a source byte no edit touched, at the
     * offset it had: the root reads its old content (S_cursor_open). */
    bool whole;
};

static bool S_cursor_copies(const markdown_core_parser *parser, const markdown_core_inline_cursor *cursor, int32_t from,
                            int32_t to, int64_t delta);

/* The cursor's levels hold `stem` next, its first node at `at`. */
static bool S_cursor_push(markdown_core_inline_cursor *cursor, const markdown_core_stem *stem, int32_t at) {
    inline_level *levels = markdown_core_reserve(cursor->levels, &cursor->capacity, cursor->depth + 1, sizeof(*levels));
    if (!levels) {
        return false;
    }
    cursor->levels = levels;
    levels[cursor->depth++] = (inline_level){stem, 0, markdown_core_stem_count(stem), at};
    return true;
}

static void S_cursor_free(markdown_core_inline_cursor *cursor) {
    if (cursor) {
        markdown_core_free(cursor->old_pieces);
        markdown_core_free(cursor->new_pieces);
        markdown_core_free(cursor->levels);
        markdown_core_free(cursor);
    }
}

/* The pieces of the old root's published runs, measured from `anchor`. */
static bool S_old_pieces(markdown_core_inline_cursor *cursor, const markdown_core_runs *runs, uint32_t anchor) {
    const markdown_core_run_piece *pieces = markdown_core_runs_pieces(runs);
    cursor->old_pieces = markdown_core_alloc(runs->pieces, sizeof(*cursor->old_pieces));
    if (!cursor->old_pieces) {
        return false;
    }
    int64_t at = anchor;
    uint32_t content = 0;
    size_t piece = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        at += runs->items[i].run.lead;
        const int64_t end = at + runs->items[i].run.span;
        do {
            const markdown_core_run_piece read = pieces[piece];
            cursor->old_pieces[piece++] = (inline_piece){(uint32_t)at, read.span, read.decoded, content};
            content += read.decoded;
            at += read.span;
        } while (at < end);
    }
    cursor->old_count = piece;
    cursor->old_size = (int32_t)content;
    return true;
}

/* The pieces of the new root's runs, which the parse holds as places. */
static bool S_new_pieces(markdown_core_inline_cursor *cursor, const markdown_core_runs *runs) {
    const markdown_core_run_piece *pieces = markdown_core_runs_pieces(runs);
    cursor->new_pieces = markdown_core_alloc(runs->count ? runs->count : 1, sizeof(*cursor->new_pieces));
    if (!cursor->new_pieces) {
        return false;
    }
    uint32_t content = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_place place = runs->items[i].place;
        cursor->new_pieces[i] = (inline_piece){place.start, place.end - place.start, pieces[i].decoded, content};
        content += pieces[i].decoded;
    }
    cursor->new_count = runs->count;
    cursor->new_size = (int32_t)content;
    return true;
}

/* THE CURSOR OF A ROOT'S RUN over `old`, the old root whose content the
 * root's `holder` reads again, measured from `anchor`; none when the old root
 * read no content, or the cursor could not be allocated. */
static void S_cursor_open(markdown_core_inline_state *inline_state, const markdown_core_node *old, uint32_t anchor) {
    const markdown_core_runs *runs = inline_state->owner->node->runs;
    if (!old->runs || !old->runs->pieces || !runs || !runs->count) {
        return;
    }
    markdown_core_inline_cursor *cursor = markdown_core_alloc(1, sizeof(*cursor));
    if (!cursor || !S_old_pieces(cursor, old->runs, anchor) || !S_new_pieces(cursor, runs) ||
        !S_cursor_push(cursor, old->children, 0)) {
        S_cursor_free(cursor);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    cursor->whole = cursor->old_size == cursor->new_size &&
                    S_cursor_copies(inline_state->owner_parser, cursor, 0, cursor->old_size, 0);
    inline_state->cursor = cursor;
}

/* The old piece that decodes content byte `at`, which the old content has:
 * the first whose bytes end past it. */
static const inline_piece *S_old_piece(const markdown_core_inline_cursor *cursor, int32_t at) {
    size_t lo = 0, hi = cursor->old_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if ((int64_t)cursor->old_pieces[mid].content + cursor->old_pieces[mid].decoded <= at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return &cursor->old_pieces[lo];
}

/* The first new piece whose source ends past `source`, or NULL. */
static const inline_piece *S_new_piece(const markdown_core_inline_cursor *cursor, uint32_t source) {
    size_t lo = 0, hi = cursor->new_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if ((uint64_t)cursor->new_pieces[mid].source + cursor->new_pieces[mid].span <= source) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < cursor->new_count ? &cursor->new_pieces[lo] : NULL;
}

/* WHERE OLD CONTENT OFFSET `at` LIES NOW: the new content offset of the
 * first content byte read at or after the now_source of the source byte old
 * content byte `at` was read from, and the end of the new content for the end
 * of the old. It never decreases as `at` grows, which is all the cursor's
 * walk asks of it; a take asks the exact map (S_cursor_fits). */
static int32_t S_position(const markdown_core_parser *parser, const markdown_core_inline_cursor *cursor, int32_t at) {
    if (at >= cursor->old_size) {
        return cursor->new_size;
    }
    const inline_piece *old = S_old_piece(cursor, at);
    const uint32_t source = markdown_core_parser_image(
        parser, old->source + (old->span == old->decoded ? (uint32_t)(at - old->content) : 0));
    for (const inline_piece *now = S_new_piece(cursor, source); now; now++) {
        if (now == cursor->new_pieces + cursor->new_count) {
            break;
        }
        if (now->decoded) {
            const uint32_t into = now->source < source && now->span == now->decoded ? source - now->source : 0;
            return (int32_t)(now->content + into);
        }
    }
    return cursor->new_size;
}

/* WHETHER `old`, which begins at old content offset `start`, IS READ AT `pos`
 * AS IT WAS: every content byte from `back` bytes before it to `reach` bytes
 * after its end is a copy of a source byte no edit touched and continues at
 * the same distance from `pos`, the start of the content when its decisions
 * read the start, and the end when they read the end. */
static bool S_cursor_fits(const markdown_core_parser *parser, const markdown_core_inline_cursor *cursor,
                          const markdown_core_node *old, int32_t start, int32_t pos) {
    const int64_t delta = (int64_t)pos - start;
    const int64_t end = (int64_t)start + old->where.extent.span;
    const int64_t before = (int64_t)start - markdown_core_inline_entry_back(old->entry);
    const int64_t after = end + old->reach;
    if ((before < 0 && delta != 0) || (after > cursor->old_size && cursor->old_size + delta != cursor->new_size) ||
        end + delta > cursor->new_size) {
        return false;
    }
    return S_cursor_copies(parser, cursor, before < 0 ? 0 : (int32_t)before,
                           after > cursor->old_size ? cursor->old_size : (int32_t)after, delta);
}

/* Whether every old content byte in [from, to) is a copy of a source byte no
 * edit touched, read now `delta` bytes after where it was. */
static bool S_cursor_copies(const markdown_core_parser *parser, const markdown_core_inline_cursor *cursor, int32_t from,
                            int32_t to, int64_t delta) {
    if (from >= to) {
        return true;
    }
    uint32_t first = 0, last = 0;
    for (int32_t at = from; at < to;) {
        const inline_piece *old_piece = S_old_piece(cursor, at);
        if (old_piece->span != old_piece->decoded) {
            return false;
        }
        const int32_t stop = (int32_t)old_piece->content + (int32_t)old_piece->decoded < to
                                 ? (int32_t)(old_piece->content + old_piece->decoded)
                                 : to;
        const uint32_t source = old_piece->source + (uint32_t)(at - old_piece->content);
        const uint32_t now_source = markdown_core_parser_image(parser, source);
        const inline_piece *new_piece = S_new_piece(cursor, now_source);
        if (!new_piece || new_piece->source > now_source || new_piece->span != new_piece->decoded ||
            (int64_t)now_source + (stop - at) > (int64_t)new_piece->source + new_piece->span ||
            (int64_t)new_piece->content + (now_source - new_piece->source) != at + delta) {
            return false;
        }
        if (at == from) {
            first = source;
        }
        last = source + (uint32_t)(stop - at) - 1;
        at = stop;
    }
    return !markdown_core_parser_touched(parser, first, last);
}

/* THE OLD NODE THE CURSOR OFFERS AT `pos`, at `*start` in the old content:
 * the cursor passes the old nodes that begin before `pos`, reading into
 * each that holds it, and offers the node it places at `pos` when a parse
 * may take that node; NULL when the next begins after `pos`. */
static const markdown_core_node *S_cursor_offer(markdown_core_inline_state *inline_state, int32_t pos, int32_t *start) {
    markdown_core_inline_cursor *cursor = inline_state->cursor;
    const markdown_core_parser *parser = inline_state->owner_parser;
    while (cursor->depth) {
        inline_level *level = &cursor->levels[cursor->depth - 1];
        if (level->next == level->count) {
            cursor->depth--;
            continue;
        }
        const markdown_core_node *node = markdown_core_stem_at(level->stem, level->next);
        const int32_t at = level->at + node->where.extent.lead;
        const int32_t end = at + (int32_t)node->where.extent.span;
        const int32_t now = S_position(parser, cursor, at);
        if (now > pos) {
            return NULL;
        }
        if (now == pos &&
            (node->entry & (cursor->whole ? MARKDOWN_CORE_INLINE_ENTRY_LOCAL : MARKDOWN_CORE_INLINE_ENTRY_TAKE))) {
            *start = at;
            return node;
        }
        level->next++;
        level->at = end;
        if (markdown_core_stem_count(node->children) && S_position(parser, cursor, end) > pos &&
            !S_cursor_push(cursor, node->children, at)) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            return NULL;
        }
    }
    return NULL;
}

/* An entry keeps one bit per delimiter rule in its low sixteen (node.h). */
typedef char inline_entry_holds_every_rule[MARKDOWN_CORE_DELIM_RULE_COUNT <= 16 ? 1 : -1];

/* THE PARSE TAKES THE OLD NODE THE CURSOR OFFERS AT THE CURSOR WHOLE, as the
 * token there (5.6), when its bytes are read at the cursor as they were, no
 * token held open here can change it, the stack holds no entry of the rules
 * its decisions counted or searched, no opener of a rule that makes its kind
 * could wrap it, and the content's owner may hold it. Its copy, which the
 * parse owns, continues it: it keeps its id and the nodes it holds, and it
 * is numbered where it lies now. The parse goes on where it ends, past the
 * whitespace boundary it left. True when it took one. */
static bool S_take_old(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    const int32_t pos = inline_state->pos;
    int32_t start = 0;
    const markdown_core_node *old = S_cursor_offer(inline_state, pos, &start);
    if (!old || pos < inline_state->opaque_end || (inline_state->token.state & MARKDOWN_CORE_INLINE_HELD) ||
        (!inline_state->cursor->whole && !S_cursor_fits(parser, inline_state->cursor, old, start, pos))) {
        return false;
    }
    const uint32_t rules = markdown_core_inline_entry_rules(old->entry);
    if (rules & inline_state->delim_rules) {
        return false;
    }
    for (int rule = MARKDOWN_CORE_DELIM_RULE_NONE + 1; rule < MARKDOWN_CORE_DELIM_RULE_COUNT; rule++) {
        const delimiter_rule_spec *spec = delimiter_spec(inline_state, (markdown_core_delimiter_rule)rule);
        if (inline_state->delim_openers[rule] && (spec->single_kind == old->kind || spec->double_kind == old->kind)) {
            return false;
        }
    }
    markdown_core_member *owner = inline_state->owner;
    if (!markdown_core_node_can_contain_type(owner->node, (markdown_core_node_type)old->kind)) {
        return false;
    }
    const int32_t end = pos + (int32_t)old->where.extent.span;
    markdown_core_node *copy = markdown_core_node_copy(parser->pool, old);
    if (!copy) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return false;
    }
    copy->id = 0;
    if (copy->runs) {
        markdown_core_node_pool_bytes_free(parser->pool, copy->runs);
        copy->runs = NULL;
    }
    copy->where.place = (markdown_core_place){(uint32_t)pos, (uint32_t)end};
    copy->reach = (uint32_t)end + old->reach;
    markdown_core_member *member = markdown_core_parser_attach(parser, owner, copy, NULL);
    if (!member) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return false;
    }
    if (copy->kind == MARKDOWN_CORE_NODE_TEXT && copy->as.literal->len > 0) {
        markdown_core_inline_map_text(inline_state, copy, pos, end - 1, inline_state->mark_cursor,
                                      inline_state->mark_cursor);
    }
    const bool boundary = (old->entry & MARKDOWN_CORE_INLINE_ENTRY_BOUNDARY) != 0;
    member->taken = member->decided = true;
    member->old = old;
    member->old_start = (uint32_t)start;
    member->passed = (uint32_t)end;
    member->reads = (markdown_core_inline_reads){
        .rules = rules,
        .state = inline_state->token.state,
        .start = pos,
        .end = end,
        .low = pos - (int32_t)markdown_core_inline_entry_back(old->entry),
        .reach = (int32_t)copy->reach,
        .until = -1,
        .flags = (old->entry & MARKDOWN_CORE_INLINE_ENTRY_TAKE ? MARKDOWN_CORE_INLINE_RECORDED : 0) |
                 MARKDOWN_CORE_INLINE_LOCAL | (boundary ? MARKDOWN_CORE_INLINE_BOUNDARY : 0)};
    if (boundary) {
        markdown_core_inline_push_boundary(inline_state, end);
    }
    inline_level *level = &inline_state->cursor->levels[inline_state->cursor->depth - 1];
    level->next++;
    level->at = start + (int32_t)old->where.extent.span;
    inline_state->pos = end;
    return true;
}

/* A token begins at the cursor: it has read nothing yet. */
static void S_begin_token(markdown_core_inline_state *inline_state) {
    bufsize_t pos = inline_state->pos;
    inline_state->token =
        (markdown_core_inline_reads){.state = S_stack_state(inline_state),
                                     .start = pos,
                                     .end = pos,
                                     .low = pos,
                                     .reach = pos,
                                     .until = -1,
                                     .flags = MARKDOWN_CORE_INLINE_RECORDED | MARKDOWN_CORE_INLINE_LOCAL};
    inline_state->token_member = NULL;
}

/* Parse an inline, advancing inline state, and add it as a child of the
 * state's OWNER. The owner used to be passed in alongside the state, and both
 * callers passed the same node on every iteration of their loop -- so the
 * owner's structural element, a pure function of its kind, was re-derived once
 * per inline token. It is resolved once now, where the owner is set. Each
 * token's constructor appends it (markdown_core_inline_state_append). */
// Return 0 if no inline can be parsed, 1 otherwise.
int markdown_core_inline_parse_inline(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    markdown_core_member *parent = inline_state->owner;
    unsigned char c;
    bufsize_t startpos, endpos;
    bufsize_t token_start = inline_state->pos;
    c = markdown_core_inline_peek_char(inline_state);
    if (c == 0) {
        return 0;
    }
    if (inline_state->records) {
        S_begin_token(inline_state);
    }
    if (inline_state->pos < inline_state->opaque_end) {
        markdown_core_inline_unrecorded(inline_state);
        startpos = inline_state->pos;
        inline_state->pos = inline_state->opaque_end;
        markdown_core_inline_state_append(
            inline_state,
            make_str(inline_state, startpos, inline_state->pos - 1,
                     markdown_core_chunk_dup(&inline_state->input, startpos, inline_state->pos - startpos)));
        goto append;
    }
    const markdown_core_element_instance *structure = inline_state->owner_structure;
    if (structure && structure->element->claim_inline_tail &&
        structure->element->claim_inline_tail(structure, inline_state, parent)) {
        return 0;
    }
    if (inline_state->cursor && S_take_old(parser, inline_state)) {
        goto append;
    }
    markdown_core_member *token = try_elements(parser, parent, c, inline_state);
    if (inline_state->pos == token_start && !token && !parser->error && !inline_state->error) {
        endpos = inline_state_find_special_char(inline_state);
        if (inline_state->pos < inline_state->text_end && endpos > inline_state->text_end) {
            endpos = inline_state->text_end;
        }
        const markdown_core_element_instance *text = parser->dialect->text_structure;
        inline_state->declared = false;
        text->element->parse_text(text, parser, inline_state, endpos);
        if (!inline_state->declared) {
            markdown_core_inline_outside(inline_state);
        }
    }
append:
    if (inline_state->records && inline_state->token_member && !parser->error && !inline_state->error) {
        /* A later parse that takes the node whole goes on where the token
         * ended, so a node that does not lie on its token's bytes is read
         * again. The root's nodes are placed at offsets of its content. */
        const markdown_core_place place = inline_state->token_member->node->where.place;
        inline_state->token.end = inline_state->pos;
        if (place.start != (uint32_t)token_start || place.end != (uint32_t)inline_state->pos) {
            markdown_core_inline_unrecorded(inline_state);
        }
        inline_state->token_member->reads = inline_state->token;
    }
    inline_state->token_member = NULL;
    endpos = inline_state->pos;
    while (endpos > token_start) {
        unsigned char byte = inline_state->input.data[--endpos];
        parser->footnote_body_work++;
        if (!markdown_core_is_space_or_tab(byte)) {
            inline_state->nonblank_end = endpos + 1;
            break;
        }
    }
    return inline_state->error != MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED;
}

const markdown_core_key *markdown_core_inline_ask(markdown_core_inline_state *inline_state,
                                                  markdown_core_key_group group, const markdown_core_chunk *label,
                                                  bool normalize) {
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_registry *registry = parser->registry;
    /* The question is the root's: a later parse asks it again, so no parse
     * takes the token that asked it whole. */
    markdown_core_inline_outside(inline_state);
    const unsigned char *bytes = label->data;
    uint32_t length = (uint32_t)label->len;
    bool failed = false;
    if (normalize) {
        if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH) {
            return NULL;
        }
        const markdown_core_strbuf *read = markdown_core_registry_normalize(registry, label, &failed);
        if (!read) {
            inline_state->error = failed ? MARKDOWN_CORE_PARSE_ALLOCATION_FAILED : inline_state->error;
            return NULL;
        }
        bytes = read->ptr;
        length = (uint32_t)read->size;
    }
    const markdown_core_key *key;
    const bool defined = markdown_core_registry_ask(registry, parser->asker, group, bytes, length, &key, &failed);
    if (failed) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return defined ? key : NULL;
}

void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_member *owner, bool root,
                                        markdown_core_inline_state *inline_state) {
    markdown_core_node *parent = owner->node;
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    /* EVERY content-bearing block has a map by the time its inlines are parsed.
     * One the parser fed line by line already does; one whose content was SET
     * -- a table cell, a directive's label -- gets one mark here, derived from
     * where the block says it starts. That derivation IS the arithmetic this
     * replaces: `start + internal_offset` was the block offset every inline
     * position used to be measured from, and stating it once as a mark is
     * what lets the term itself go. */
    if (parent->content_map.count == 0) {
        markdown_core_parser_mark_content(parser, parent, 0, parent->where.place.start + parent->internal_offset);
    }
    markdown_core_inline_state_from_buf(parser, inline_state, &content);
    /* Block buffers include their terminating line ending. An inline field
     * ends at its owner's delimiter: its trailing spaces are body content. */
    if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P(parent->kind)) {
        markdown_core_chunk_rtrim(&inline_state->input);
    }
    /* A root's nodes are placed in its content: the bytes this parse reads
     * keep their map to the source as the root's runs. A field's nodes are
     * placed in the content of the root it was cut from, which its map is a
     * slice of. */
    if (root) {
        markdown_core_parser_read_content(parser, parent, inline_state->input.len);
    }
    inline_state->owner = owner;
    inline_state->owner_structure = markdown_core_parser_structure(parser, parent);
    inline_state->mark_cursor = parent->content_map.first;
    markdown_core_inline_seat_cursor(inline_state);

    const markdown_core_element_instance *structure = inline_state->owner_structure;
    if (structure && structure->element->begin_inline && S_inline_run_began(inline_state)) {
        structure->element->begin_inline(structure, parser, inline_state, owner);
    }
}

void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state) {
    markdown_core_parser *parser = inline_state->owner_parser;
    if (S_inline_run_began(inline_state)) {
        const markdown_core_element_instance *const *owners =
            parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
        size_t dispose_count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
        for (size_t i = 0; i < dispose_count; i++) {
            parser->inline_hook_work++;
            owners[i]->element->dispose_inline(owners[i], inline_state);
        }
    }
    if (inline_state->run_state) {
        S_give_inline_records(parser, inline_state->run_state);
        inline_state->run_state = NULL;
    }
    inline_state->now = INT32_MAX;
    while (inline_state->last_delim) {
        markdown_core_inline_remove_delimiter(inline_state, inline_state->last_delim);
    }
    S_cursor_free(inline_state->cursor);
    inline_state->cursor = NULL;
    if (inline_state->error) {
        markdown_core_parser_fail(parser, inline_state->error);
    }
}

bool markdown_core_inline_finish_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    while (!parser->error && !inline_state->error) {
        complete_inline_token(parser, inline_state);
        if (parser->error || inline_state->error || markdown_core_inline_is_eof(inline_state) ||
            !markdown_core_inline_parse_inline(parser, inline_state)) {
            break;
        }
    }
    if (!parser->error && !inline_state->error) {
        const markdown_core_element_instance *const *owners =
            parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_FINISH];
        size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_FINISH];
        for (size_t i = 0; i < count; i++) {
            parser->inline_hook_work++;
            owners[i]->element->finish_inline(owners[i], inline_state);
        }
        markdown_core_inline_process_delimiters(parser, inline_state, 0, NULL);
    }
    bool whitespace = inline_state->last_delim && inline_state->last_delim->kind == DELIMITER_BOUNDARY;
    markdown_core_inline_clear_inlines(inline_state);
    return whitespace;
}

bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_member *parent) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, parent, false, &inline_state);
    return markdown_core_inline_finish_inlines(parser, &inline_state);
}

void markdown_core_parse_root_inlines(markdown_core_parser *parser, markdown_core_member *holder,
                                      const markdown_core_node *old, uint32_t anchor) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, holder, true, &inline_state);
    inline_state.records = true;
    parser->stay_count = 0;
    if (old && !inline_state.error) {
        S_cursor_open(&inline_state, old, anchor);
    }
    markdown_core_inline_finish_inlines(parser, &inline_state);
}

void markdown_core_inline_state_set_opaque_body_end(markdown_core_inline_state *inline_state, int end) {
    inline_state->opaque_end = end;
}

int markdown_core_inline_state_find_opaque_close(markdown_core_inline_state *inline_state,
                                                 markdown_core_delimiter_rule rule, int from,
                                                 markdown_core_opaque_delimiter_scanner scan) {
    if (inline_state->opaque_failed_from[rule] && from >= inline_state->opaque_failed_from[rule] - 1) {
        return -1;
    }
    for (int at = from; at < inline_state->input.len;) {
        bool closes = false;
        int width = scan(inline_state->input.data, inline_state->input.len, at, rule, &closes);
        inline_state->owner_parser->opaque_scan_work++;
        if (closes) {
            return at;
        }
        at += width;
    }
    inline_state->opaque_failed_from[rule] = from + 1;
    return -1;
}

unsigned char markdown_core_inline_state_peek_char(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_peek_char(inline_state);
}

unsigned char markdown_core_inline_state_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos) {
    return markdown_core_inline_peek_at(inline_state, pos);
}

int markdown_core_inline_state_is_eof(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_is_eof(inline_state);
}

void markdown_core_inline_state_read(markdown_core_inline_state *inline_state, int from, int to) {
    inline_state->declared = true;
    from = from < 0 ? -1 : from;
    to = to > inline_state->input.len ? inline_state->input.len + 1 : to;
    inline_state->token.low = from < inline_state->token.low ? from : inline_state->token.low;
    inline_state->token.reach = to > inline_state->token.reach ? to : inline_state->token.reach;
}

static char *my_strndup(const char *s, size_t n) {
    char *result;
    size_t len = strlen(s);

    if (n < len) {
        len = n;
    }

    result = (char *)malloc(len + 1);
    if (!result) {
        return 0;
    }

    result[len] = '\0';
    return (char *)memcpy(result, s, len);
}

char *markdown_core_inline_state_take_while(markdown_core_inline_state *inline_state,
                                            markdown_core_inline_predicate pred) {
    unsigned char c;
    bufsize_t startpos = inline_state->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(inline_state)) && (*pred)(c)) {
        advance(inline_state);
        len++;
    }

    return my_strndup((const char *)inline_state->input.data + startpos, len);
}

void markdown_core_inline_state_push_delimiter(markdown_core_inline_state *inline_state,
                                               const markdown_core_element_instance *owner,
                                               markdown_core_delimiter_rule rule, int can_open, int can_close,
                                               markdown_core_member *inl_text) {
    push_delimiter(inline_state, owner, rule, can_open != 0, can_close != 0, inl_text);
}

void markdown_core_inline_state_advance_offset(markdown_core_inline_state *inline_state) { advance(inline_state); }

int markdown_core_inline_state_get_offset(markdown_core_inline_state *inline_state) { return inline_state->pos; }

// Moving the cursor over a consumed span must move the line counter with it.
//
// This used to be an assignment and nothing else, so an element that consumed
// a span containing a line ending left `line` and `column_offset` where they
// were: its own node reported a column on the START line, and every later node
// in the same paragraph was displaced by the same amount. It is the same defect
// `adjust_subj_node_newlines` fixes for the core's own spans; the only
// difference is that an element names a destination offset where the core
// names a match length.
//
/* `autolink` rewinds through here and every element advances through it. The
 * cursor is the only thing that moves: a position is asked of the map when a
 * node is made, so there is no line or column frame left to keep in step. */
void markdown_core_inline_state_set_offset(markdown_core_inline_state *inline_state, int offset) {
    inline_state->pos = offset;
}

markdown_core_node *markdown_core_inline_state_make_delimiter_text(markdown_core_inline_state *inline_state, int from,
                                                                   int to) {
    markdown_core_node *node;

    if (from < 0 || to < from || to >= inline_state->input.len) {
        return NULL;
    }
    node = markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_TEXT, from, to,
                                             markdown_core_chunk_dup(&inline_state->input, from, to - from + 1));
    return node;
}

markdown_core_chunk *markdown_core_inline_state_get_chunk(markdown_core_inline_state *inline_state) {
    return &inline_state->input;
}

static void S_update_text_sourcepos(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->as.literal->len == 0) {
        node->where.place.end = node->where.place.start;
        return;
    }
    int line;
    bufsize_t end;
    markdown_core_parser_content_end_place(parser, &node->content_map, node->as.literal->len - 1, &line, &end);
    node->where.place.end = (uint32_t)end;
}

void markdown_core_node_unput(markdown_core_parser *parser, markdown_core_member *member, int n) {
    for (markdown_core_member *text = member->last; n > 0 && text && text->node->kind == MARKDOWN_CORE_NODE_TEXT;
         text = text->prev) {
        markdown_core_node *node = text->node;
        bufsize_t remove = node->as.literal->len < (bufsize_t)n ? node->as.literal->len : (bufsize_t)n;
        node->as.literal->len -= remove;
        n -= (int)remove;
        S_update_text_sourcepos(parser, node);
        /* What follows the Text decided its end. */
        text->reads.flags &= ~(uint32_t)MARKDOWN_CORE_INLINE_RECORDED;
    }
}

int markdown_core_inline_state_has_unmatched_opener(markdown_core_inline_state *inline_state,
                                                    markdown_core_delimiter_rule rule) {
    if (rule <= MARKDOWN_CORE_DELIM_RULE_NONE || rule >= MARKDOWN_CORE_DELIM_RULE_COUNT) {
        return 0;
    }
    markdown_core_inline_read_rule(inline_state, rule);
    /* Counts kept at push and removal, so this is one comparison however deep
     * the stack is. For a rule whose closers are pushed only when this says
     * yes, every closer on the stack has an opener below it, and an opener is
     * unmatched exactly when the openers outnumber the closers. */
    return inline_state->delim_openers[rule] > inline_state->delim_closers[rule];
}

markdown_core_member *markdown_core_delimiter_member(const delimiter *delim) { return delim->member; }

markdown_core_delimiter_rule markdown_core_delimiter_rule_of(const delimiter *delim) { return delim->rule; }

bufsize_t markdown_core_delimiter_position(const delimiter *delim) { return delim->position; }

bufsize_t markdown_core_delimiter_length(const delimiter *delim) { return delim->length; }

int markdown_core_delimiter_can_open(const delimiter *delim) { return delim->can_open; }

int markdown_core_delimiter_can_close(const delimiter *delim) { return delim->can_close; }

/* A bare token scanner must leave the active container's closer to the
 * bracket algorithm. Unlike link/image labels, footnote bodies allow links. */

markdown_core_member *markdown_core_inline_match_delimiter(const markdown_core_element_instance *self,
                                                           markdown_core_inline_state *inline_state) {
    const delimiter_run *run = scan_delimiter(inline_state, inline_state->pos, self->element->delimiter_rule);
    if (!delimiter_needs_stack(inline_state, run)) {
        if (!self->element->delimiter.exact_run) {
            return NULL;
        }
        inline_state->pos = run->end;
        return markdown_core_inline_state_append(
            inline_state, make_str(inline_state, run->start, run->end - 1,
                                   markdown_core_chunk_dup(&inline_state->input, run->start, run->end - run->start)));
    }
    return handle_delim(inline_state, run);
}
