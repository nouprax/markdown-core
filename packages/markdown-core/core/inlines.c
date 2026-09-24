#include "alloc.h"
#include "inline_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "markdown_core_ctype.h"
#include "config.h"
#include "node.h"
#include "parser.h"
#include "references.h"
#include "map.h"
#include "markdown-core.h"
#include "houdini.h"
#include "utf8.h"
#include "delimiter.h"
#include "inlines.h"
#include "element.h"
#include "../elements/markdown-core-elements.h"

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
                                      inline_state->owner ? &inline_state->owner->content_map : NULL, from, to, &span,
                                      &inline_state->mark_cursor);
    markdown_core_inline_seat_cursor(inline_state);
    if (span.has_start) {
        node->start_line = span.start_line;
        node->start_column = span.start_column;
    }
    if (span.has_end) {
        node->end_line = span.end_line;
        node->end_column = span.end_column;
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
                markdown_core_parser_append_content_mark(inline_state->owner_parser, node, 0, node->start_line,
                                                         node->start_column, node->end_column - node->start_column + 1,
                                                         0);
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

void markdown_core_inline_state_from_buf(markdown_core_parser *parser, int line_number,
                                         markdown_core_inline_state *inline_state, markdown_core_chunk *chunk,
                                         markdown_core_map *refmap) {
    memset(inline_state, 0, sizeof(*inline_state));
    inline_state->input = *chunk;
    inline_state->line = line_number;
    inline_state->owner_parser = parser;
    inline_state->refmap = refmap;
    inline_state->text_end = -1;
    if (parser) {
        inline_state->dialect = parser->dialect;
        const markdown_core_element *const *owners = parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_INIT];
        size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_INIT];
        for (size_t i = 0; i < count; i++) {
            parser->inline_hook_work++;
            owners[i]->init_inline(inline_state);
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
    while (markdown_core_inline_peek_char(inline_state) == ' ' ||
           markdown_core_inline_peek_char(inline_state) == '\t') {
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
    const markdown_core_element *owner = inline_state->dialect->delimiter_owners[rule];
    static const delimiter_rule_spec empty = {0};
    return owner ? &owner->delimiter : &empty;
}

/* Classify without moving the parser cursor or allocating an AST node.
 * A cached lookahead is keyed by its source offset, so text scanning and
 * delimiter dispatch consume the same classification even after a rewind. */
static const delimiter_run *scan_delimiter(markdown_core_inline_state *inline_state, bufsize_t start,
                                           markdown_core_delimiter_rule rule) {
    if (inline_state->cached_run.rule != MARKDOWN_CORE_DELIM_RULE_NONE && inline_state->cached_run.start == start &&
        inline_state->cached_run.rule == rule) {
        return &inline_state->cached_run;
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
    if (spec->body == DELIMITER_WORD_BODY) {
        run.can_open = run.can_close = true;
        inline_state->cached_run = run;
        return &inline_state->cached_run;
    }
    if (run.end - run.start < spec->minimum_width || (spec->exact_run && run.end - run.start != spec->minimum_width)) {
        inline_state->cached_run = run;
        return &inline_state->cached_run;
    }

    bufsize_t before_char_pos, after_char_pos;
    int32_t after_char = 0, before_char = 0;
    int len;
    if (run.start == 0) {
        before_char = 10;
    } else {
        before_char_pos = run.start - 1;
        // Walk back to the beginning of the UTF-8 sequence.
        while ((markdown_core_inline_peek_at(inline_state, before_char_pos) >> 6 == 2 ||
                inline_state->dialect->skip_chars[markdown_core_inline_peek_at(inline_state, before_char_pos)]) &&
               before_char_pos > 0) {
            before_char_pos--;
        }
        len = markdown_core_utf8proc_iterate(inline_state->input.data + before_char_pos, run.start - before_char_pos,
                                             &before_char);
        if (len == -1 || (before_char < 256 && inline_state->dialect->skip_chars[(unsigned char)before_char])) {
            before_char = 10;
        }
    }
    if (run.end == inline_state->input.len) {
        after_char = 10;
    } else {
        after_char_pos = run.end;
        while (after_char_pos < inline_state->input.len && flanking_skip_at(inline_state, after_char_pos)) {
            after_char_pos++;
        }
        len = markdown_core_utf8proc_iterate(inline_state->input.data + after_char_pos,
                                             inline_state->input.len - after_char_pos, &after_char);
        if (len == -1 || (after_char < 256 && inline_state->dialect->skip_chars[(unsigned char)after_char])) {
            after_char = 10;
        }
    }
    bool left_flanking =
        !markdown_core_utf8proc_is_space(after_char) &&
        (!markdown_core_utf8proc_is_punctuation_or_symbol(after_char) || markdown_core_utf8proc_is_space(before_char) ||
         markdown_core_utf8proc_is_punctuation_or_symbol(before_char));
    bool right_flanking =
        !markdown_core_utf8proc_is_space(before_char) &&
        (!markdown_core_utf8proc_is_punctuation_or_symbol(before_char) || markdown_core_utf8proc_is_space(after_char) ||
         markdown_core_utf8proc_is_punctuation_or_symbol(after_char));
    if (spec->punctuation_bound) {
        run.can_open =
            left_flanking && (!right_flanking || markdown_core_utf8proc_is_punctuation_or_symbol(before_char));
        run.can_close =
            right_flanking && (!left_flanking || markdown_core_utf8proc_is_punctuation_or_symbol(after_char));
    } else {
        run.can_open = left_flanking;
        run.can_close = right_flanking;
    }
    inline_state->cached_run = run;
    return &inline_state->cached_run;
}

/* Source classification is immutable, but eligibility depends on the live
 * stack. A close-only run cannot match a future opener. Keep it as text when
 * no earlier opener of its rule survives; runs that can open must remain
 * eligible even without an earlier opener. Counts are conservative because
 * pair reduction is deferred and one run can supply several delimiter units. */
static bool delimiter_needs_stack(const markdown_core_inline_state *inline_state, const delimiter_run *run) {
    return run->can_open || (run->can_close && inline_state->delim_openers[run->rule] > 0);
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
 * without (`markdown_core_parse_reference_inline`) scans a label and pushes
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
    bool whitespace = markdown_core_parse_inline_subtrees(parser, entry->node, inline_state->refmap);
    if (whitespace) {
        entry->kind = DELIMITER_BOUNDARY;
        entry->node = NULL;
        if (entry->previous && entry->previous->kind == DELIMITER_BOUNDARY) {
            markdown_core_inline_remove_delimiter(inline_state, entry->previous);
        }
    } else {
        markdown_core_inline_remove_delimiter(inline_state, entry);
    }
}

static void push_delimiter(markdown_core_inline_state *inline_state, const markdown_core_element *owner,
                           markdown_core_delimiter_rule rule, bool can_open, bool can_close,
                           markdown_core_node *inl_text) {
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
    delim->node = inl_text;
    delim->length = inl_text->as.literal->len;
    if (can_open) {
        inline_state->delim_openers[rule]++;
    }
    if (can_close) {
        inline_state->delim_closers[rule]++;
    }
}

static markdown_core_node *handle_delim(markdown_core_inline_state *inline_state, const delimiter_run *run) {
    assert(delimiter_needs_stack(inline_state, run));
    inline_state->pos = run->end;
    markdown_core_node *inl_text =
        make_str(inline_state, run->start, run->end - 1,
                 markdown_core_chunk_dup(&inline_state->input, run->start, run->end - run->start));
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
        const markdown_core_element *element = closer->owner;
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
                    if (opener->node->as.literal->len < used || closer->node->as.literal->len < used) {
                        used = spec->minimum_width;
                    }
                    markdown_core_node_type kind = used == 2 ? spec->double_kind : spec->single_kind;
                    closer = S_insert_delimited_inline(inline_state, opener, closer, used, kind);
                } else if (element && element->insert_inline_from_delim) {
                    delimiter *next = closer->next;
                    element->insert_inline_from_delim(element, parser, inline_state, opener, closer);
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
    reduce_delimiter_range(inline_state, candidate, after);
}

static delimiter *S_insert_delimited_inline(markdown_core_inline_state *inline_state, delimiter *opener,
                                            delimiter *closer, bufsize_t use_delims, markdown_core_node_type kind) {
    delimiter *tmp_delim;
    markdown_core_node *opener_inl = opener->node;
    markdown_core_node *closer_inl = closer->node;
    bufsize_t opener_num_chars = opener_inl->as.literal->len;
    bufsize_t closer_num_chars = closer_inl->as.literal->len;
    markdown_core_node *tmp, *tmpnext, *inline_node;
    const bufsize_t minimum_width = delimiter_spec(inline_state, closer->rule)->minimum_width;

    /* A rejected container leaves its authored text intact for every rule. */
    if (!markdown_core_node_can_contain_type(opener_inl->parent, kind)) {
        delimiter *next = closer->next;
        markdown_core_inline_remove_delimiter(inline_state, opener);
        markdown_core_inline_remove_delimiter(inline_state, closer);
        return next;
    }

    // Allocate before mutating either run. OOM leaves the source intact and
    // aborts the shared parse transaction.
    inline_node = markdown_core_inline_make_simple(inline_state, kind);
    if (!inline_node) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return closer->next;
    }

    inline_node->element = closer->owner;

    // remove used characters from associated inlines.
    opener_num_chars -= use_delims;
    closer_num_chars -= use_delims;
    opener_inl->as.literal->len = opener_num_chars;
    closer_inl->as.literal->len = closer_num_chars;

    tmp = opener_inl->next;
    if (tmp && tmp != closer_inl) {
        inline_node->first_child = tmp;
        tmp->prev = NULL;

        while (tmp && tmp != closer_inl) {
            tmpnext = tmp->next;
            if (inline_state->owner_parser) {
                inline_state->owner_parser->delimiter_work++;
            }
            tmp->parent = inline_node;
            if (tmpnext == closer_inl) {
                inline_node->last_child = tmp;
                tmp->next = NULL;
            }
            tmp = tmpnext;
        }
    }

    opener_inl->next = inline_node;
    closer_inl->prev = inline_node;
    inline_node->prev = opener_inl;
    inline_node->next = closer_inl;
    inline_node->parent = opener_inl->parent;

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
    inline_node->start_line = opener_inl->start_line;
    inline_node->end_line = closer_inl->end_line;
    inline_node->start_column = opener_inl->start_column + (int)opener_num_chars;
    inline_node->end_column = closer_inl->end_column - (int)closer_num_chars;
    // and a leftover that SURVIVES owns only the bytes it still carries. A
    // leftover with none is freed below, and writing its end first would put a
    // reversed range in the tree for the length of two statements -- true only
    // by reading ahead, which is not a property worth relying on.
    if (opener_num_chars > 0) {
        opener_inl->end_column = opener_inl->start_column + (int)opener_num_chars - 1;
    }
    if (closer_num_chars > 0) {
        closer_inl->content_map.offset += (int)use_delims;
        closer_inl->start_column = closer_inl->end_column - (int)closer_num_chars + 1;
    }

    // if opener has 0 characters, remove it and its associated inline
    if (opener_num_chars == 0) {
        markdown_core_parser_release_node(inline_state->owner_parser, opener_inl);
        markdown_core_inline_remove_delimiter(inline_state, opener);
    } else if (opener_num_chars < minimum_width) {
        markdown_core_inline_remove_delimiter(inline_state, opener); // A remaining single sign is only text.
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
        // remove empty closer inline
        markdown_core_parser_release_node(inline_state->owner_parser, closer_inl);
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

static bufsize_t inline_state_find_special_char(markdown_core_inline_state *inline_state) {
    // The caller has already established that the first byte is literal.
    // The dialect is sealed, so its tables are read through one local.
    const markdown_core_dialect *const dialect = inline_state->dialect;
    bufsize_t n = inline_state->pos;
    while (n < inline_state->input.len) {
        unsigned char c = inline_state->input.data[n];
        if (!dialect->special_chars[c]) {
            n++;
        } else if (dialect->inline_start_predicates[c] && !dialect->inline_start_predicates[c](inline_state, n)) {
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

static markdown_core_node *try_elements(markdown_core_parser *parser, markdown_core_node *parent, unsigned char c,
                                        markdown_core_inline_state *inline_state) {
    markdown_core_node *res = NULL;
    bufsize_t start = inline_state->pos;
    /* The dialect is sealed, so the byte's owner list is read once. */
    const markdown_core_dialect *dialect = parser->dialect;
    const markdown_core_element *const *owner = dialect->inline_dispatch + dialect->inline_dispatch_offsets[c];
    const markdown_core_element *const *end = dialect->inline_dispatch + dialect->inline_dispatch_offsets[c + 1];

    for (; owner < end; owner++) {
        const markdown_core_element *element = *owner;
        res = element->match_inline(element, parser, parent, c, inline_state);

        if (res || inline_state->pos != start || parser->error || inline_state->error) {
            break;
        }
    }

    return res;
}

static int has_inline_field(markdown_core_node **root_slot, void *context) {
    if (root_slot && *root_slot) {
        *(bool *)context = true;
    }
    return 1;
}

/* Parse an inline, advancing inline state, and add it as a child of the
 * state's OWNER. The owner used to be passed in alongside the state, and both
 * callers passed the same node on every iteration of their loop -- so the
 * owner's structural element, a pure function of its kind, was re-derived once
 * per inline token. It is resolved once now, where the owner is set. */
// Return 0 if no inline can be parsed, 1 otherwise.
int markdown_core_inline_parse_inline(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    markdown_core_node *parent = inline_state->owner;
    markdown_core_node *new_inl = NULL;
    unsigned char c;
    bufsize_t startpos, endpos;
    bufsize_t token_start = inline_state->pos;
    c = markdown_core_inline_peek_char(inline_state);
    if (c == 0) {
        return 0;
    }
    if (inline_state->pos < inline_state->opaque_end) {
        startpos = inline_state->pos;
        inline_state->pos = inline_state->opaque_end;
        new_inl = make_str(inline_state, startpos, inline_state->pos - 1,
                           markdown_core_chunk_dup(&inline_state->input, startpos, inline_state->pos - startpos));
        goto append;
    }
    const markdown_core_element *structure = inline_state->owner_structure;
    if (structure && structure->claim_inline_tail && structure->claim_inline_tail(inline_state, parent)) {
        return 0;
    }
    new_inl = try_elements(parser, parent, c, inline_state);
    if (inline_state->pos == token_start && !new_inl && !parser->error && !inline_state->error) {
        endpos = inline_state_find_special_char(inline_state);
        if (inline_state->pos < inline_state->text_end && endpos > inline_state->text_end) {
            endpos = inline_state->text_end;
        }
        new_inl = parser->dialect->text_structure->parse_text(parser, inline_state, endpos);
    }
append:
    endpos = inline_state->pos;
    while (endpos > token_start) {
        unsigned char byte = inline_state->input.data[--endpos];
        parser->footnote_body_work++;
        if (byte != ' ' && byte != '\t') {
            inline_state->nonblank_end = endpos + 1;
            break;
        }
    }
    if (new_inl != NULL) {
        /* Fixed inline owners admit every token their grammar constructs. Only
         * a dynamic policy needs a decision here; it may change during a hook. */
        if (parent->element && parent->element->can_contain_func &&
            !markdown_core_node_can_contain_type(parent, (markdown_core_node_type)new_inl->kind)) {
            /* A token constructor has consumed input. Refusing its result is
             * a failed parse, but the detached token is still ours to release. */
            markdown_core_parser_release_node(parser, new_inl);
            if (!inline_state->error) {
                inline_state->error = MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED;
            }
            return 0;
        }
        markdown_core_node_attach_validated(parent, new_inl, NULL);
        bool has_fields = false;
        markdown_core_visit_inline_subtrees(new_inl, has_inline_field, &has_fields);
        if (has_fields) {
            delimiter *entry =
                markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_FIELD, inline_state->pos);
            if (entry) {
                entry->node = new_inl;
            }
        }
    }
    return 1;
}

void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent,
                                        markdown_core_map *refmap, markdown_core_inline_state *inline_state) {
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    /* EVERY content-bearing block has a map by the time its inlines are parsed.
     * One the parser fed line by line already does; one whose content was SET
     * -- a table cell, a directive's label -- gets one mark here, derived from
     * where the block says it starts. That derivation IS the arithmetic this
     * replaces: `start_column - 1 + internal_offset` was the block offset every
     * inline position used to be measured from, and stating it once as a mark
     * is what lets the term itself go. */
    if (parent->content_map.count == 0) {
        markdown_core_parser_mark_content(parser, parent, parent->start_line,
                                          parent->start_column + parent->internal_offset);
    }
    markdown_core_inline_state_from_buf(parser, parent->start_line, inline_state, &content, refmap);
    inline_state->owner = parent;
    inline_state->owner_structure = markdown_core_node_structure(parent);
    inline_state->mark_cursor = parent->content_map.first;
    markdown_core_inline_seat_cursor(inline_state);
    /* Block buffers include their terminating line ending. An inline field
     * ends at its owner's delimiter: its trailing spaces are body content. */
    if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P(parent->kind)) {
        markdown_core_chunk_rtrim(&inline_state->input);
    }

    const markdown_core_element *structure = inline_state->owner_structure;
    if (structure && structure->begin_inline) {
        structure->begin_inline(parser, inline_state, parent);
    }
}

void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state) {
    markdown_core_parser *parser = inline_state->owner_parser;
    const markdown_core_element *const *owners = parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
    size_t dispose_count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
    for (size_t i = 0; i < dispose_count; i++) {
        parser->inline_hook_work++;
        owners[i]->dispose_inline(inline_state);
    }
    while (inline_state->last_delim) {
        markdown_core_inline_remove_delimiter(inline_state, inline_state->last_delim);
    }
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
        const markdown_core_element *const *owners = parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_FINISH];
        size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_FINISH];
        for (size_t i = 0; i < count; i++) {
            parser->inline_hook_work++;
            owners[i]->finish_inline(inline_state);
        }
        markdown_core_inline_process_delimiters(parser, inline_state, 0, NULL);
    }
    bool whitespace = inline_state->last_delim && inline_state->last_delim->kind == DELIMITER_BOUNDARY;
    markdown_core_inline_clear_inlines(inline_state);
    return whitespace;
}

bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, parent, refmap, &inline_state);
    return markdown_core_inline_finish_inlines(parser, &inline_state);
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
                                               const markdown_core_element *owner, markdown_core_delimiter_rule rule,
                                               int can_open, int can_close, markdown_core_node *inl_text) {
    push_delimiter(inline_state, owner, rule, can_open != 0, can_close != 0, inl_text);
}

int markdown_core_inline_state_scan_delimiters(markdown_core_inline_state *inline_state, int max_delims,
                                               unsigned char c, int *left_flanking, int *right_flanking,
                                               int *punct_before, int *punct_after) {
    int numdelims = 0;
    bufsize_t before_char_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool space_before, space_after;

    if (inline_state->pos == 0) {
        before_char = 10;
    } else {
        before_char_pos = inline_state->pos - 1;
        // walk back to the beginning of the UTF_8 sequence:
        while (markdown_core_inline_peek_at(inline_state, before_char_pos) >> 6 == 2 && before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(inline_state->input.data + before_char_pos,
                                             inline_state->pos - before_char_pos, &before_char);
        if (len == -1) {
            before_char = 10;
        }
    }

    while (markdown_core_inline_peek_char(inline_state) == c && numdelims < max_delims) {
        numdelims++;
        advance(inline_state);
    }

    len = markdown_core_utf8proc_iterate(inline_state->input.data + inline_state->pos,
                                         inline_state->input.len - inline_state->pos, &after_char);
    if (len == -1) {
        after_char = 10;
    }

    *punct_before = markdown_core_utf8proc_is_punctuation_or_symbol(before_char);
    *punct_after = markdown_core_utf8proc_is_punctuation_or_symbol(after_char);
    space_before = markdown_core_utf8proc_is_space(before_char) != 0;
    space_after = markdown_core_utf8proc_is_space(after_char) != 0;

    *left_flanking = numdelims > 0 && !markdown_core_utf8proc_is_space(after_char) &&
                     !(*punct_after && !space_before && !*punct_before);
    *right_flanking = numdelims > 0 && !markdown_core_utf8proc_is_space(before_char) &&
                      !(*punct_before && !space_after && !*punct_after);

    return numdelims;
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

/* The cursor's position, asked of the map from the cursor's run. */
static int S_cursor_place(markdown_core_inline_state *inline_state, int *line, int *column) {
    markdown_core_content_span span;
    int placed = markdown_core_parser_content_span(inline_state->owner_parser,
                                                   inline_state->owner ? &inline_state->owner->content_map : NULL,
                                                   inline_state->pos, -1, &span, &inline_state->mark_cursor);
    markdown_core_inline_seat_cursor(inline_state);
    if (!placed || !span.has_start) {
        return 0;
    }
    *line = span.start_line;
    *column = span.start_column;
    return 1;
}

int markdown_core_inline_state_get_column(markdown_core_inline_state *inline_state) {
    int line, column;
    if (S_cursor_place(inline_state, &line, &column)) {
        return column;
    }
    return inline_state->pos + 1;
}

markdown_core_chunk *markdown_core_inline_state_get_chunk(markdown_core_inline_state *inline_state) {
    return &inline_state->input;
}

static void S_update_text_sourcepos(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->as.literal->len == 0) {
        node->start_line = node->start_column = node->end_line = node->end_column = 0;
        return;
    }
    markdown_core_parser_content_end_place(parser, &node->content_map, node->as.literal->len - 1, &node->end_line,
                                           &node->end_column);
}

void markdown_core_node_unput(markdown_core_parser *parser, markdown_core_node *node, int n) {
    node = node->last_child;
    while (n > 0 && node && node->kind == MARKDOWN_CORE_NODE_TEXT) {
        bufsize_t remove = node->as.literal->len < (bufsize_t)n ? node->as.literal->len : (bufsize_t)n;
        node->as.literal->len -= remove;
        n -= (int)remove;
        S_update_text_sourcepos(parser, node);
        node = node->prev;
    }
}

int markdown_core_inline_state_has_unmatched_opener(markdown_core_inline_state *inline_state,
                                                    markdown_core_delimiter_rule rule) {
    if (rule <= MARKDOWN_CORE_DELIM_RULE_NONE || rule >= MARKDOWN_CORE_DELIM_RULE_COUNT) {
        return 0;
    }
    /* Counts kept at push and removal, so this is one comparison however deep
     * the stack is. For a rule whose closers are pushed only when this says
     * yes, every closer on the stack has an opener below it, and an opener is
     * unmatched exactly when the openers outnumber the closers. */
    return inline_state->delim_openers[rule] > inline_state->delim_closers[rule];
}

int markdown_core_inline_state_get_line(markdown_core_inline_state *inline_state) {
    int line, column;
    if (S_cursor_place(inline_state, &line, &column)) {
        return line;
    }
    return inline_state->line;
}

markdown_core_node *markdown_core_delimiter_node(const delimiter *delim) { return delim->node; }

markdown_core_delimiter_rule markdown_core_delimiter_rule_of(const delimiter *delim) { return delim->rule; }

bufsize_t markdown_core_delimiter_position(const delimiter *delim) { return delim->position; }

bufsize_t markdown_core_delimiter_length(const delimiter *delim) { return delim->length; }

int markdown_core_delimiter_can_open(const delimiter *delim) { return delim->can_open; }

int markdown_core_delimiter_can_close(const delimiter *delim) { return delim->can_close; }

/* A bare token scanner must leave the active container's closer to the
 * bracket algorithm. Unlike link/image labels, footnote bodies allow links. */

markdown_core_node *markdown_core_inline_match_delimiter(const markdown_core_element *element,
                                                         markdown_core_inline_state *inline_state) {
    const delimiter_run *run = scan_delimiter(inline_state, inline_state->pos, element->delimiter_rule);
    if (!delimiter_needs_stack(inline_state, run)) {
        if (!element->delimiter.exact_run) {
            return NULL;
        }
        inline_state->pos = run->end;
        return make_str(inline_state, run->start, run->end - 1,
                        markdown_core_chunk_dup(&inline_state->input, run->start, run->end - run->start));
    }
    return handle_delim(inline_state, run);
}
