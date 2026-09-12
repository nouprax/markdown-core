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
#include "extension.h"
#include "../extensions/markdown-core-extensions.h"

static const int8_t EMPTY_CHAR_SET[256] = {0};
static markdown_core_delimiter_rule delimiter_rule_for_byte(subject *subj, unsigned char c) {
    return subj->owner_parser ? subj->owner_parser->delimiter_chars[c] : MARKDOWN_CORE_DELIM_RULE_NONE;
}

static delimiter *S_insert_delimited_inline(subject *subj, delimiter *opener, delimiter *closer, bufsize_t use_delims,
                                            markdown_core_node_type kind);

static bufsize_t subject_find_special_char(subject *subj);

/* Give `node` the source extent of the content bytes [from, to].
 *
 * THIS IS THE WHOLE OF THE INLINE POSITION MODEL: a position is a PROJECTION
 * of the byte range a node covers, asked of requirement 10's content-to-source
 * map, and not a counter each handler keeps in step. The two arguments every
 * maker already took were byte offsets into the block's content buffer and
 * were being turned into columns by addition -- which is right only while the
 * whole block is one line that starts where the block does. Everything the old
 * arithmetic needed a correction term for -- a span that crosses a line
 * ending, a continuation line with a different stripped prefix, a construct
 * whose end is on a later line than its start -- is answered by asking twice.
 *
 * The fallback is the old arithmetic and it is reached by a block whose
 * content was SET rather than fed: a table cell, the paragraph a table was
 * split out of, a reference definition parsed straight out of a chunk. Those
 * have no marks to project through, and Step 8's second half is where they get
 * them. */
void markdown_core_inline_parser_place(subject *subj, markdown_core_node *node, int from, int to) {
    int line, column;

    /* Every content-bearing block has a map by the time its inlines are parsed
     * -- `markdown_core_parse_inlines` gives one to any block whose content was
     * SET rather than fed -- so there is no arithmetic left to fall back to.
     * The subject built straight out of a chunk by
     * `markdown_core_parse_reference_inline` has no owner and creates no nodes,
     * which is why the miss below leaves the position at calloc's zero rather
     * than guessing. */
    if (markdown_core_parser_content_place(subj->owner_parser, subj->owner, from, &line, &column)) {
        node->start_line = line;
        node->start_column = column;
    }
    if (markdown_core_parser_content_end_place(subj->owner_parser, subj->owner, to, &line, &column)) {
        node->end_line = line;
        node->end_column = column;
    }
    if (node->kind == MARKDOWN_CORE_NODE_TEXT && node->as.literal->len > 0 && subj->owner) {
        /* Copied bytes take a view of the source map; a decoded source token
         * maps each of its output bytes to that token's authored extent. */
        if (node->as.literal->len == to - from + 1 &&
            memcmp(node->as.literal->data, subj->input.data + from, (size_t)node->as.literal->len) == 0) {
            markdown_core_parser_adopt_content_marks(subj->owner_parser, subj->owner, node, from, to - from + 1);
        } else {
            node->content_mark_count = 0;
            node->content_mark_offset = 0;
            markdown_core_parser_append_content_mark(subj->owner_parser, node, 0, node->start_line, node->start_column,
                                                     node->end_column - node->start_column + 1, 0);
        }
    }
}

// Create an inline with a literal string value.
markdown_core_node *markdown_core_inline_make_literal(subject *subj, markdown_core_node_type t, int start_column,
                                                      int end_column, markdown_core_chunk s) {
    markdown_core_node *e = markdown_core_node_new_with_mem(t, subj->mem);
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(subj->mem, &s);
        subj->oom = 1;
        return NULL;
    }
    *e->as.literal = s;
    markdown_core_inline_parser_place(subj, e, start_column, end_column);
    return e;
}

// Create an inline with no value.
markdown_core_node *markdown_core_inline_make_simple(markdown_core_mem *mem, markdown_core_node_type t) {
    return markdown_core_node_new_with_mem(t, mem);
}

/* markdown_core_inline_make_simple with the subject's loss flag for handlers that consume input
 * before creating the node. */
markdown_core_node *markdown_core_inline_make_simple_subj(subject *subj, markdown_core_node_type t) {
    markdown_core_node *e = markdown_core_inline_make_simple(subj->mem, t);
    if (!e) {
        subj->oom = 1;
    }
    return e;
}

// Like markdown_core_node_append_child but without costly sanity checks.
// Assumes that child was newly created.
void markdown_core_inline_append_child(markdown_core_node *node, markdown_core_node *child) {
    markdown_core_node *old_last_child = node->last_child;

    child->next = NULL;
    child->prev = old_last_child;
    child->parent = node;
    node->last_child = child;

    if (old_last_child) {
        old_last_child->next = child;
    } else {
        // Also set first_child if node previously had no children.
        node->first_child = child;
    }
}

void markdown_core_inline_subject_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number,
                                           subject *e, markdown_core_chunk *chunk, markdown_core_map *refmap) {
    memset(e, 0, sizeof(*e));
    e->special_chars = parser ? parser->special_chars : EMPTY_CHAR_SET;
    e->skip_chars = parser ? parser->skip_chars : EMPTY_CHAR_SET;
    e->mem = mem;
    e->input = *chunk;
    e->line = line_number;
    e->owner_parser = parser;
    e->refmap = refmap;
    e->text_end = -1;
    if (parser) {
        for (markdown_core_llist *entry = parser->inline_lifecycle_extensions; entry; entry = entry->next) {
            const markdown_core_extension *syntax = entry->data;
            if (syntax->init_inline) {
                syntax->init_inline(e);
            }
        }
    }
}

unsigned char markdown_core_inline_peek_char_n(subject *subj, bufsize_t n) {
    // NULL bytes should have been stripped out by now.  If they're
    // present, it's a programming error:
    assert(!(subj->pos + n < subj->input.len && subj->input.data[subj->pos + n] == 0));
    return (subj->pos + n < subj->input.len) ? subj->input.data[subj->pos + n] : 0;
}

unsigned char markdown_core_inline_peek_char(subject *subj) { return markdown_core_inline_peek_char_n(subj, 0); }

unsigned char markdown_core_inline_peek_at(subject *subj, bufsize_t pos) { return subj->input.data[pos]; }

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
static MARKDOWN_CORE_INLINE unsigned char flanking_skip_at(subject *subj, bufsize_t pos) {
    assert(pos < subj->input.len);
    return subj->skip_chars[markdown_core_inline_peek_at(subj, pos)];
}

// Return true if there are more characters in the subject.
int markdown_core_inline_is_eof(subject *subj) { return (subj->pos >= subj->input.len); }

// Advance the subject.  Doesn't check for eof.
#define advance(subj) (subj)->pos += 1

bool markdown_core_inline_skip_spaces(subject *subj) {
    bool skipped = false;
    while (markdown_core_inline_peek_char(subj) == ' ' || markdown_core_inline_peek_char(subj) == '\t') {
        advance(subj);
        skipped = true;
    }
    return skipped;
}

bool markdown_core_inline_skip_line_end(subject *subj) {
    bool seen_line_end_char = false;
    if (markdown_core_inline_peek_char(subj) == '\r') {
        advance(subj);
        seen_line_end_char = true;
    }
    if (markdown_core_inline_peek_char(subj) == '\n') {
        advance(subj);
        seen_line_end_char = true;
    }
    return seen_line_end_char || markdown_core_inline_is_eof(subj);
}

// Take characters while a predicate holds, and return a string.
static const delimiter_rule_spec *delimiter_spec(subject *subj, markdown_core_delimiter_rule rule) {
    const markdown_core_extension *owner = subj->owner_parser ? subj->owner_parser->delimiter_owners[rule] : NULL;
    static const delimiter_rule_spec empty = {0};
    return owner ? &owner->delimiter : &empty;
}

/* Classify without moving the parser cursor or allocating an AST node.
 * A cached lookahead is keyed by its source offset, so text scanning and
 * delimiter dispatch consume the same classification even after a rewind. */
static const delimiter_run *scan_delimiter(subject *subj, bufsize_t start, markdown_core_delimiter_rule rule) {
    if (subj->cached_run.rule != MARKDOWN_CORE_DELIM_RULE_NONE && subj->cached_run.start == start &&
        subj->cached_run.rule == rule) {
        return &subj->cached_run;
    }
    unsigned char c = markdown_core_inline_peek_at(subj, start);
    delimiter_run run = {.start = start, .end = start, .rule = rule};
    assert(run.rule != MARKDOWN_CORE_DELIM_RULE_NONE);
    const delimiter_rule_spec *spec = delimiter_spec(subj, run.rule);
    while (run.end < subj->input.len && markdown_core_inline_peek_at(subj, run.end) == c &&
           (!spec->run_limit || run.end - run.start < spec->run_limit)) {
        run.end++;
        if (subj->owner_parser) {
            subj->owner_parser->delimiter_work++;
        }
    }
    if (spec->body == DELIMITER_WORD_BODY) {
        run.can_open = run.can_close = true;
        subj->cached_run = run;
        return &subj->cached_run;
    }
    if (run.end - run.start < spec->minimum_width || (spec->exact_run && run.end - run.start != spec->minimum_width)) {
        subj->cached_run = run;
        return &subj->cached_run;
    }

    bufsize_t before_char_pos, after_char_pos;
    int32_t after_char = 0, before_char = 0;
    int len;
    if (run.start == 0) {
        before_char = 10;
    } else {
        before_char_pos = run.start - 1;
        // Walk back to the beginning of the UTF-8 sequence.
        while ((markdown_core_inline_peek_at(subj, before_char_pos) >> 6 == 2 ||
                subj->skip_chars[markdown_core_inline_peek_at(subj, before_char_pos)]) &&
               before_char_pos > 0) {
            before_char_pos--;
        }
        len = markdown_core_utf8proc_iterate(subj->input.data + before_char_pos, run.start - before_char_pos,
                                             &before_char);
        if (len == -1 || (before_char < 256 && subj->skip_chars[(unsigned char)before_char])) {
            before_char = 10;
        }
    }
    if (run.end == subj->input.len) {
        after_char = 10;
    } else {
        after_char_pos = run.end;
        while (after_char_pos < subj->input.len && flanking_skip_at(subj, after_char_pos)) {
            after_char_pos++;
        }
        len = markdown_core_utf8proc_iterate(subj->input.data + after_char_pos, subj->input.len - after_char_pos,
                                             &after_char);
        if (len == -1 || (after_char < 256 && subj->skip_chars[(unsigned char)after_char])) {
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
    subj->cached_run = run;
    return &subj->cached_run;
}

/* Source classification is immutable, but eligibility depends on the live
 * stack. A close-only run cannot match a future opener. Keep it as text when
 * no earlier opener of its rule survives; runs that can open must remain
 * eligible even without an earlier opener. Counts are conservative because
 * pair reduction is deferred and one run can supply several delimiter units. */
static bool delimiter_needs_stack(const subject *subj, const delimiter_run *run) {
    return run->can_open || (run->can_close && subj->delim_openers[run->rule] > 0);
}

/*
static void print_delimiters(subject *subj)
{
        delimiter *delim;
        delim = subj->last_delim;
        while (delim != NULL) {
                printf("Item at stack pos %p: %d %d %d next(%p) prev(%p)\n",
                       (void*)delim, (int)delim->rule,
                       delim->can_open, delim->can_close,
                       (void*)delim->next, (void*)delim->previous);
                delim = delim->previous;
        }
}
*/

void markdown_core_inline_remove_delimiter(subject *subj, delimiter *delim) {
    if (delim == NULL) {
        return;
    }
    if (delim->next == NULL) {
        // end of list:
        assert(delim == subj->last_delim);
        subj->last_delim = delim->previous;
    } else {
        delim->next->previous = delim->previous;
    }
    if (delim->previous != NULL) {
        delim->previous->next = delim->next;
    }
    if (delim->can_open) {
        subj->delim_openers[delim->rule]--;
    }
    if (delim->can_close) {
        subj->delim_closers[delim->rule]--;
    }
    subj->mem->free(delim);
}

delimiter *markdown_core_inline_push_delimiter_entry(subject *subj, delimiter_kind kind, bufsize_t position) {
    delimiter *entry = (delimiter *)subj->mem->calloc(1, sizeof(delimiter));
    if (!entry) {
        subj->oom = 1;
        return NULL;
    }
    entry->kind = kind;
    entry->position = position;
    entry->previous = subj->last_delim;
    if (entry->previous) {
        entry->previous->next = entry;
    }
    subj->last_delim = entry;
    return entry;
}

void markdown_core_inline_push_boundary(subject *subj, bufsize_t position) {
    if (subj->last_delim && subj->last_delim->kind == DELIMITER_BOUNDARY) {
        subj->last_delim->position = position;
    } else {
        markdown_core_inline_push_delimiter_entry(subj, DELIMITER_BOUNDARY, position);
    }
}

/* Reduce a completed range to its last content boundary. Markers cannot
 * escape a completed container, but its whitespace still constrains an
 * enclosing word body. Keeping one summary also bounds repeated work across
 * nested bracket scopes: each removed entry is visited only once. */
static void reduce_delimiter_range(subject *subj, delimiter *before, delimiter *after) {
    delimiter *entry = after ? after->previous : subj->last_delim;
    bool boundary = false;
    while (entry != before) {
        delimiter *previous = entry->previous;
        assert(entry->kind != DELIMITER_FIELD);
        if (subj->owner_parser) {
            subj->owner_parser->delimiter_work++;
        }
        if (entry->kind == DELIMITER_BOUNDARY && !boundary) {
            boundary = true;
        } else {
            markdown_core_inline_remove_delimiter(subj, entry);
        }
        entry = previous;
    }
}

/* A token's owned fields finish before scanning its successor, preserving
 * reference occurrence order. Heading declaration can suspend with this
 * event on the same stack and resume after its symbol table is complete. */
static void complete_inline_token(markdown_core_parser *parser, subject *subj) {
    delimiter *entry = subj->last_delim;
    if (!entry || entry->kind != DELIMITER_FIELD) {
        return;
    }
    bool whitespace = markdown_core_parse_inline_subtrees(parser, entry->node, subj->refmap);
    if (whitespace) {
        entry->kind = DELIMITER_BOUNDARY;
        entry->node = NULL;
        if (entry->previous && entry->previous->kind == DELIMITER_BOUNDARY) {
            markdown_core_inline_remove_delimiter(subj, entry->previous);
        }
    } else {
        markdown_core_inline_remove_delimiter(subj, entry);
    }
}

static void push_delimiter(subject *subj, const markdown_core_extension *owner, markdown_core_delimiter_rule rule,
                           bool can_open, bool can_close, markdown_core_node *inl_text) {
    delimiter *delim;
    /* Extensions may pass NULL after their own allocation failures. */
    if (!inl_text) {
        subj->oom = 1;
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
    delim = markdown_core_inline_push_delimiter_entry(subj, DELIMITER_MARKER, subj->pos);
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
        subj->delim_openers[rule]++;
    }
    if (can_close) {
        subj->delim_closers[rule]++;
    }
}

static markdown_core_node *handle_delim(subject *subj, const delimiter_run *run) {
    assert(delimiter_needs_stack(subj, run));
    subj->pos = run->end;
    markdown_core_node *inl_text = make_str(subj, run->start, run->end - 1,
                                            markdown_core_chunk_dup(&subj->input, run->start, run->end - run->start));
    // One eligible maximal run owns one stack entry and cannot match itself.
    if (inl_text) {
        push_delimiter(subj, subj->owner_parser ? subj->owner_parser->delimiter_owners[run->rule] : NULL, run->rule,
                       run->can_open, run->can_close, inl_text);
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

void markdown_core_inline_process_delimiters(markdown_core_parser *parser, subject *subj, bufsize_t stack_bottom,
                                             delimiter *after) {
    delimiter *candidate;
    delimiter *closer = after;
    delimiter *opener;
    delimiter *old_closer;
    bool opener_found;
    /* One slot per RULE, so the array is sized by construction. It used to be
     * `[3][128]` indexed by a byte the public push accepts unconstrained. */
    bufsize_t openers_bottom[3][MARKDOWN_CORE_DELIM_RULE_COUNT];
    int i;

    // initialize openers_bottom:
    for (i = 0; i < 3; i++) {
        for (int rule = 0; rule < MARKDOWN_CORE_DELIM_RULE_COUNT; rule++) {
            openers_bottom[i][rule] = stack_bottom;
        }
    }

    // move back to first relevant delim.
    candidate = after ? after->previous : subj->last_delim;
    while (candidate != NULL && candidate->position >= stack_bottom) {
        closer = candidate;
        candidate = candidate->previous;
    }

    // now move forward, looking for closers, and handling each
    //
    // EVERY ARM ADVANCES `closer`, and that is D33. The old chain was
    // `if (extension) ... else if (delim_char is * or _) ... else if (' or ")`,
    // so a delimiter that matched none of the three -- which is what a byte
    // whose owner cannot be found looks like -- left `closer` where it was,
    // fell into the removal below, freed it, and read it again on the next
    // turn. With `can_open` set, nothing freed it and the loop never ended.
    // The quote arm is gone with smart punctuation: a quotation mark is
    // ordinary text and pushes no delimiter.
    while (closer != after) {
        const markdown_core_extension *extension = closer->owner;
        if (closer->kind == DELIMITER_BOUNDARY || closer->kind == DELIMITER_AFFIX_BOUNDARY) {
            for (int rule = 0; rule < MARKDOWN_CORE_DELIM_RULE_COUNT; rule++) {
                if (closer->kind == DELIMITER_AFFIX_BOUNDARY ||
                    delimiter_spec(subj, rule)->body == DELIMITER_WORD_BODY) {
                    for (i = 0; i < 3; i++) {
                        openers_bottom[i][rule] = closer->position;
                    }
                }
            }
        }
        assert(closer->kind != DELIMITER_FIELD);
        if (closer->can_close) {
            // Now look backwards for first matching opener:
            opener = closer->previous;
            opener_found = false;
            while (opener != NULL && opener->position >= stack_bottom &&
                   opener->position >= openers_bottom[closer->length % 3][closer->rule]) {
                if (subj->owner_parser) {
                    subj->owner_parser->delimiter_work++;
                }
                if (opener->can_open && opener->rule == closer->rule) {
                    // interior closer of size 2 can't match opener of size 1
                    // or of size 1 can't match 2
                    if (!delimiter_spec(subj, closer->rule)->rule_of_three ||
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
                reduce_delimiter_range(subj, opener, closer);
                const delimiter_rule_spec *spec = delimiter_spec(subj, closer->rule);
                if (spec->minimum_width) {
                    bufsize_t used = spec->maximum_width;
                    if (opener->node->as.literal->len < used || closer->node->as.literal->len < used) {
                        used = spec->minimum_width;
                    }
                    markdown_core_node_type kind = used == 2 ? spec->double_kind : spec->single_kind;
                    closer = S_insert_delimited_inline(subj, opener, closer, used, kind);
                } else if (extension && extension->insert_inline_from_delim) {
                    delimiter *next = closer->next;
                    extension->insert_inline_from_delim(extension, parser, subj, opener, closer);
                    markdown_core_inline_remove_delimiter(subj, opener);
                    markdown_core_inline_remove_delimiter(subj, closer);
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
                    markdown_core_inline_remove_delimiter(subj, old_closer);
                }
            }
        } else {
            closer = closer->next;
        }
    }
    reduce_delimiter_range(subj, candidate, after);
}

static delimiter *S_insert_delimited_inline(subject *subj, delimiter *opener, delimiter *closer, bufsize_t use_delims,
                                            markdown_core_node_type kind) {
    delimiter *tmp_delim;
    markdown_core_node *opener_inl = opener->node;
    markdown_core_node *closer_inl = closer->node;
    bufsize_t opener_num_chars = opener_inl->as.literal->len;
    bufsize_t closer_num_chars = closer_inl->as.literal->len;
    markdown_core_node *tmp, *tmpnext, *inline_node;
    const bufsize_t minimum_width = delimiter_spec(subj, closer->rule)->minimum_width;

    /* A rejected container leaves its authored text intact for every rule. */
    if (!markdown_core_node_can_contain_type(opener_inl->parent, kind)) {
        delimiter *next = closer->next;
        markdown_core_inline_remove_delimiter(subj, opener);
        markdown_core_inline_remove_delimiter(subj, closer);
        return next;
    }

    // Allocate before mutating either run. OOM leaves the source intact and
    // aborts the shared parse transaction.
    inline_node = markdown_core_inline_make_simple(subj->mem, kind);
    if (!inline_node) {
        subj->oom = 1;
        return closer->next;
    }

    inline_node->extension = closer->owner;

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
            if (subj->owner_parser) {
                subj->owner_parser->delimiter_work++;
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
        closer_inl->content_mark_offset += (int)use_delims;
        closer_inl->start_column = closer_inl->end_column - (int)closer_num_chars + 1;
    }

    // if opener has 0 characters, remove it and its associated inline
    if (opener_num_chars == 0) {
        markdown_core_node_free(opener_inl);
        markdown_core_inline_remove_delimiter(subj, opener);
    } else if (opener_num_chars < minimum_width) {
        markdown_core_inline_remove_delimiter(subj, opener); // A remaining single sign is only text.
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
        // remove empty closer inline
        markdown_core_node_free(closer_inl);
        // remove closer from list
        tmp_delim = closer->next;
        markdown_core_inline_remove_delimiter(subj, closer);
        closer = tmp_delim;
    } else if (closer_num_chars < minimum_width) {
        tmp_delim = closer->next;
        markdown_core_inline_remove_delimiter(subj, closer);
        closer = tmp_delim;
    }

    return closer;
}

static bufsize_t subject_find_special_char(subject *subj) {
    // The caller has already established that the first byte is literal.
    bufsize_t n = subj->pos;
    while (n < subj->input.len) {
        unsigned char c = subj->input.data[n];
        if (!subj->special_chars[c]) {
            n++;
        } else if (subj->owner_parser && subj->owner_parser->inline_start_predicates[c] &&
                   !subj->owner_parser->inline_start_predicates[c](subj, n)) {
            n++;
        } else if (delimiter_rule_for_byte(subj, c) != MARKDOWN_CORE_DELIM_RULE_NONE) {
            const delimiter_run *run = scan_delimiter(subj, n, delimiter_rule_for_byte(subj, c));
            if (delimiter_needs_stack(subj, run)) {
                assert(n > subj->pos);
                return n;
            }
            // A run that cannot delimit belongs to the current text slice.
            n = run->end;
        } else if (n > subj->pos) {
            return n;
        } else {
            n++;
        }
    }
    return subj->input.len;
}

void markdown_core_inlines_reset_special_chars(markdown_core_parser *parser) {
    memset(parser->special_chars, 0, sizeof(parser->special_chars));
    memset(parser->skip_chars, 0, sizeof(parser->skip_chars));
}
void markdown_core_inlines_add_text_terminator(markdown_core_parser *parser, unsigned char c) {
    parser->special_chars[c] = 1;
}
void markdown_core_inlines_remove_text_terminator(markdown_core_parser *parser, unsigned char c) {
    parser->special_chars[c] = 0;
}
void markdown_core_inlines_add_flanking_transparent(markdown_core_parser *parser, unsigned char c) {
    parser->skip_chars[c] = 1;
}
void markdown_core_inlines_remove_flanking_transparent(markdown_core_parser *parser, unsigned char c) {
    parser->skip_chars[c] = 0;
}

static markdown_core_node *try_extensions(markdown_core_parser *parser, markdown_core_node *parent, unsigned char c,
                                          subject *subj) {
    markdown_core_node *res = NULL;
    bufsize_t start = subj->pos;

    for (size_t i = parser->inline_dispatch_offsets[c]; i < parser->inline_dispatch_offsets[c + 1]; i++) {
        const markdown_core_extension *ext = parser->inline_dispatch[i];
        res = ext->match_inline(ext, parser, parent, c, subj);

        if (res || subj->pos != start || parser->oom || subj->oom) {
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

// Parse an inline, advancing subject, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
int markdown_core_inline_parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent) {
    markdown_core_node *new_inl = NULL;
    unsigned char c;
    bufsize_t startpos, endpos;
    bufsize_t token_start = subj->pos;
    c = markdown_core_inline_peek_char(subj);
    if (c == 0) {
        return 0;
    }
    if (subj->pos < subj->opaque_end) {
        startpos = subj->pos;
        subj->pos = subj->opaque_end;
        new_inl = make_str(subj, startpos, subj->pos - 1,
                           markdown_core_chunk_dup(&subj->input, startpos, subj->pos - startpos));
        goto append;
    }
    const markdown_core_extension *syntax = markdown_core_node_syntax(parent);
    if (syntax && syntax->claim_inline_tail && syntax->claim_inline_tail(subj, parent)) {
        return 0;
    }
    new_inl = try_extensions(parser, parent, c, subj);
    if (subj->pos == token_start && !new_inl && !parser->oom && !subj->oom) {
        endpos = subject_find_special_char(subj);
        if (subj->pos < subj->text_end && endpos > subj->text_end) {
            endpos = subj->text_end;
        }
        new_inl = parser->text_syntax->parse_text(parser, subj, endpos);
    }
append:
    endpos = subj->pos;
    while (endpos > token_start) {
        unsigned char byte = subj->input.data[--endpos];
        parser->footnote_body_work++;
        if (byte != ' ' && byte != '\t') {
            subj->nonblank_end = endpos + 1;
            break;
        }
    }
    if (new_inl != NULL) {
        markdown_core_inline_append_child(parent, new_inl);
        bool has_fields = false;
        markdown_core_visit_inline_subtrees(new_inl, has_inline_field, &has_fields);
        if (has_fields) {
            delimiter *entry = markdown_core_inline_push_delimiter_entry(subj, DELIMITER_FIELD, subj->pos);
            if (entry) {
                entry->node = new_inl;
            }
        }
    }
    return 1;
}

void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent,
                                        markdown_core_map *refmap, subject *subj) {
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    /* EVERY content-bearing block has a map by the time its inlines are parsed.
     * One the parser fed line by line already does; one whose content was SET
     * -- a table cell, a directive's label -- gets one mark here, derived from
     * where the block says it starts. That derivation IS the arithmetic this
     * replaces: `start_column - 1 + internal_offset` was the block offset every
     * inline position used to be measured from, and stating it once as a mark
     * is what lets the term itself go. */
    if (parent->content_mark_count == 0) {
        markdown_core_parser_mark_content(parser, parent, parent->start_line,
                                          parent->start_column + parent->internal_offset);
    }
    markdown_core_inline_subject_from_buf(parser, parser->mem, parent->start_line, subj, &content, refmap);
    subj->owner = parent;
    /* Block buffers include their terminating line ending. An inline field
     * ends at its owner's delimiter: its trailing spaces are body content. */
    if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P(parent->kind)) {
        markdown_core_chunk_rtrim(&subj->input);
    }

    const markdown_core_extension *syntax = markdown_core_node_syntax(parent);
    if (syntax && syntax->begin_inline) {
        syntax->begin_inline(parser, subj, parent);
    }
}

void markdown_core_inline_clear_inlines(subject *subj) {
    markdown_core_parser *parser = subj->owner_parser;
    for (markdown_core_llist *entry = parser->inline_lifecycle_extensions; entry; entry = entry->next) {
        const markdown_core_extension *syntax = entry->data;
        if (syntax->dispose_inline) {
            syntax->dispose_inline(subj);
        }
    }
    while (subj->last_delim) {
        markdown_core_inline_remove_delimiter(subj, subj->last_delim);
    }
    if (subj->oom) {
        parser->oom = true;
    }
}

bool markdown_core_inline_finish_inlines(markdown_core_parser *parser, subject *subj) {
    while (!parser->oom && !subj->oom) {
        complete_inline_token(parser, subj);
        if (parser->oom || subj->oom || markdown_core_inline_is_eof(subj) ||
            !markdown_core_inline_parse_inline(parser, subj, subj->owner)) {
            break;
        }
    }
    if (!parser->oom && !subj->oom) {
        for (markdown_core_llist *entry = parser->inline_lifecycle_extensions; entry; entry = entry->next) {
            const markdown_core_extension *syntax = entry->data;
            if (syntax->finish_inline) {
                syntax->finish_inline(subj);
            }
        }
        markdown_core_inline_process_delimiters(parser, subj, 0, NULL);
    }
    bool whitespace = subj->last_delim && subj->last_delim->kind == DELIMITER_BOUNDARY;
    markdown_core_inline_clear_inlines(subj);
    return whitespace;
}

bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap) {
    subject subj;
    markdown_core_inline_start_inlines(parser, parent, refmap, &subj);
    return markdown_core_inline_finish_inlines(parser, &subj);
}

void markdown_core_inline_parser_set_opaque_body_end(markdown_core_inline_parser *parser, int end) {
    parser->opaque_end = end;
}

int markdown_core_inline_parser_find_opaque_close(markdown_core_inline_parser *parser,
                                                  markdown_core_delimiter_rule rule, int from,
                                                  markdown_core_opaque_delimiter_scanner scan) {
    if (parser->opaque_failed_from[rule] && from >= parser->opaque_failed_from[rule] - 1) {
        return -1;
    }
    for (int at = from; at < parser->input.len;) {
        bool closes = false;
        int width = scan(parser->input.data, parser->input.len, at, rule, &closes);
        parser->owner_parser->opaque_scan_work++;
        if (closes) {
            return at;
        }
        at += width;
    }
    parser->opaque_failed_from[rule] = from + 1;
    return -1;
}

unsigned char markdown_core_inline_parser_peek_char(markdown_core_inline_parser *parser) {
    return markdown_core_inline_peek_char(parser);
}

unsigned char markdown_core_inline_parser_peek_at(markdown_core_inline_parser *parser, bufsize_t pos) {
    return markdown_core_inline_peek_at(parser, pos);
}

int markdown_core_inline_parser_is_eof(markdown_core_inline_parser *parser) {
    return markdown_core_inline_is_eof(parser);
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

char *markdown_core_inline_parser_take_while(markdown_core_inline_parser *parser, markdown_core_inline_predicate pred) {
    unsigned char c;
    bufsize_t startpos = parser->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(parser)) && (*pred)(c)) {
        advance(parser);
        len++;
    }

    return my_strndup((const char *)parser->input.data + startpos, len);
}

void markdown_core_inline_parser_push_delimiter(markdown_core_inline_parser *parser,
                                                const markdown_core_extension *owner, markdown_core_delimiter_rule rule,
                                                int can_open, int can_close, markdown_core_node *inl_text) {
    push_delimiter(parser, owner, rule, can_open != 0, can_close != 0, inl_text);
}

int markdown_core_inline_parser_scan_delimiters(markdown_core_inline_parser *parser, int max_delims, unsigned char c,
                                                int *left_flanking, int *right_flanking, int *punct_before,
                                                int *punct_after) {
    int numdelims = 0;
    bufsize_t before_char_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool space_before, space_after;

    if (parser->pos == 0) {
        before_char = 10;
    } else {
        before_char_pos = parser->pos - 1;
        // walk back to the beginning of the UTF_8 sequence:
        while (markdown_core_inline_peek_at(parser, before_char_pos) >> 6 == 2 && before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(parser->input.data + before_char_pos, parser->pos - before_char_pos,
                                             &before_char);
        if (len == -1) {
            before_char = 10;
        }
    }

    while (markdown_core_inline_peek_char(parser) == c && numdelims < max_delims) {
        numdelims++;
        advance(parser);
    }

    len =
        markdown_core_utf8proc_iterate(parser->input.data + parser->pos, parser->input.len - parser->pos, &after_char);
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

void markdown_core_inline_parser_advance_offset(markdown_core_inline_parser *parser) { advance(parser); }

int markdown_core_inline_parser_get_offset(markdown_core_inline_parser *parser) { return parser->pos; }

// Moving the cursor over a consumed span must move the line counter with it.
//
// This used to be an assignment and nothing else, so an extension that consumed
// a span containing a line ending left `line` and `column_offset` where they
// were: its own node reported a column on the START line, and every later node
// in the same paragraph was displaced by the same amount. It is the same defect
// `adjust_subj_node_newlines` fixes for the core's own spans; the only
// difference is that an extension names a destination offset where the core
// names a match length.
//
/* `autolink` rewinds through here and every extension advances through it. The
 * cursor is the only thing that moves: a position is asked of the map when a
 * node is made, so there is no line or column frame left to keep in step. */
void markdown_core_inline_parser_set_offset(markdown_core_inline_parser *parser, int offset) { parser->pos = offset; }

markdown_core_node *markdown_core_inline_parser_make_delimiter_text(markdown_core_inline_parser *parser, int from,
                                                                    int to) {
    markdown_core_node *node;

    if (from < 0 || to < from || to >= parser->input.len) {
        return NULL;
    }
    node = markdown_core_inline_make_literal(parser, MARKDOWN_CORE_NODE_TEXT, from, to,
                                             markdown_core_chunk_dup(&parser->input, from, to - from + 1));
    return node;
}

int markdown_core_inline_parser_get_column(markdown_core_inline_parser *parser) {
    int line, column;
    if (markdown_core_parser_content_place(parser->owner_parser, parser->owner, parser->pos, &line, &column)) {
        return column;
    }
    return parser->pos + 1;
}

markdown_core_chunk *markdown_core_inline_parser_get_chunk(markdown_core_inline_parser *parser) {
    return &parser->input;
}

static void S_update_text_sourcepos(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->as.literal->len == 0) {
        node->start_line = node->start_column = node->end_line = node->end_column = 0;
        return;
    }
    markdown_core_parser_content_end_place(parser, node, node->as.literal->len - 1, &node->end_line, &node->end_column);
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

int markdown_core_inline_parser_has_unmatched_opener(markdown_core_inline_parser *parser,
                                                     markdown_core_delimiter_rule rule) {
    if (rule <= MARKDOWN_CORE_DELIM_RULE_NONE || rule >= MARKDOWN_CORE_DELIM_RULE_COUNT) {
        return 0;
    }
    /* Counts kept at push and removal, so this is one comparison however deep
     * the stack is. For a rule whose closers are pushed only when this says
     * yes, every closer on the stack has an opener below it, and an opener is
     * unmatched exactly when the openers outnumber the closers. */
    return parser->delim_openers[rule] > parser->delim_closers[rule];
}

int markdown_core_inline_parser_get_line(markdown_core_inline_parser *parser) {
    int line, column;
    if (markdown_core_parser_content_place(parser->owner_parser, parser->owner, parser->pos, &line, &column)) {
        return line;
    }
    return parser->line;
}

markdown_core_node *markdown_core_delimiter_node(const delimiter *delim) { return delim->node; }

markdown_core_delimiter_rule markdown_core_delimiter_rule_of(const delimiter *delim) { return delim->rule; }

bufsize_t markdown_core_delimiter_position(const delimiter *delim) { return delim->position; }

bufsize_t markdown_core_delimiter_length(const delimiter *delim) { return delim->length; }

int markdown_core_delimiter_can_open(const delimiter *delim) { return delim->can_open; }

int markdown_core_delimiter_can_close(const delimiter *delim) { return delim->can_close; }

/* A bare token scanner must leave the active container's closer to the
 * bracket algorithm. Unlike link/image labels, footnote bodies allow links. */

markdown_core_node *markdown_core_inline_match_delimiter(const markdown_core_extension *extension,
                                                         markdown_core_inline_parser *subj) {
    const delimiter_run *run = scan_delimiter(subj, subj->pos, extension->delimiter_rule);
    if (!delimiter_needs_stack(subj, run)) {
        if (!extension->delimiter.exact_run) {
            return NULL;
        }
        subj->pos = run->end;
        return make_str(subj, run->start, run->end - 1,
                        markdown_core_chunk_dup(&subj->input, run->start, run->end - run->start));
    }
    return handle_delim(subj, run);
}
