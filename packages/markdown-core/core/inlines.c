#include "../extensions/comment.h"
#include "../extensions/attributes.h"
#include "../extensions/citation.h"
#include "../extensions/footnote.h"
#include "../extensions/heading.h"
#include "../extensions/link.h"
#include "../extensions/media.h"
#include "../extensions/span.h"
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
#include "scanners.h"
#include "delimiter.h"
#include "inlines.h"
#include "extension.h"
#include "../extensions/markdown-core-extensions.h"

// Macros for creating various kinds of simple.

#define MAXBACKTICKS 80

// "\r\n\\`&_*+=[]<!"
static const int8_t BASE_SPECIAL_CHARS[256] = {
    ['\n'] = 1, ['\r'] = 1, ['\\'] = 1, ['`'] = 1, ['&'] = 1, ['_'] = 1,
    ['*'] = 1,  ['['] = 1,  [']'] = 1,  ['<'] = 1, ['!'] = 1,
};

// No emphasis-boundary skip characters by default; attached inline extensions
// add theirs to the parser-local copy.
static const int8_t BASE_SKIP_CHARS[256] = {0};

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

// Like make_str, but parses entities.
static markdown_core_node *make_str_with_entities(subject *subj, int start_column, int end_column,
                                                  markdown_core_chunk *content) {
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT(subj->mem);

    if (houdini_unescape_html(&unescaped, content->data, content->len)) {
        if (unescaped.oom) {
            subj->oom = 1;
        }
        return make_str(subj, start_column, end_column, markdown_core_chunk_buf_detach(&unescaped));
    } else {
        return make_str(subj, start_column, end_column, *content);
    }
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

static markdown_core_chunk markdown_core_clean_autolink(subject *subj, markdown_core_chunk *url, int is_email) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        markdown_core_chunk result = MARKDOWN_CORE_CHUNK_EMPTY;
        return result;
    }

    if (is_email) {
        markdown_core_strbuf_puts(&buf, "mailto:");
    }

    houdini_unescape_html_f(&buf, url->data, url->len);
    if (buf.oom) {
        subj->oom = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

static MARKDOWN_CORE_INLINE markdown_core_node *make_autolink(subject *subj, int start_column, int end_column,
                                                              markdown_core_chunk url, int is_email) {
    markdown_core_node *link = markdown_core_inline_make_simple(subj->mem, MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *text;
    if (!link) {
        subj->oom = 1;
        return NULL;
    }
    {
        // No title here: an autolink has no syntax for one, so the resource is
        // built with absence. It used to be set to an empty title, and
        // `extensions.txt` records both spellings of one construct on one line
        // disagreeing about it three columns apart.
        markdown_core_chunk destination = markdown_core_clean_autolink(subj, &url, is_email);
        link->as.link->resource =
            markdown_core_resource_new(subj->mem, destination, markdown_core_optional_chunk_absent());
        if (!link->as.link->resource) {
            subj->oom = 1;
            markdown_core_chunk_free(subj->mem, &destination);
            markdown_core_node_free(link);
            return NULL;
        }
    }

    // Both offsets, like every other column in this file. This was the one site
    // that turned a raw subject-buffer offset into a column without them, so an
    // autolink inside a block quote, or on any line but the first of its
    // paragraph, produced a Link that did not contain its own Text.
    markdown_core_inline_parser_place(subj, link, start_column, end_column);
    text = make_str_with_entities(subj, start_column + 1, end_column - 1, &url);
    if (text) {
        markdown_core_inline_append_child(link, text);
    }
    markdown_core_inline_attach_inline_attributes(subj, link, start_column);
    /* The pointy braces are the syntax; what they enclose is the text. */
    return link;
}

void markdown_core_inline_subject_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number,
                                           subject *e, markdown_core_chunk *chunk, markdown_core_map *refmap) {
    e->special_chars = parser ? parser->special_chars : BASE_SPECIAL_CHARS;
    e->skip_chars = parser ? parser->skip_chars : BASE_SKIP_CHARS;
    e->mem = mem;
    e->input = *chunk;
    memset(&e->attributes, 0, sizeof(e->attributes));
    e->heading_attributes_start = e->heading_content_end = -1;
    e->flags = 0;
    e->opaque_end = 0;
    memset(e->opaque_failed_from, 0, sizeof(e->opaque_failed_from));
    e->line = line_number;
    e->pos = 0;
    e->owner_parser = parser;
    e->owner = NULL;
    e->refmap = refmap;
    e->last_delim = NULL;
    e->cached_run = (delimiter_run){0};
    memset(e->delim_openers, 0, sizeof(e->delim_openers));
    memset(e->delim_closers, 0, sizeof(e->delim_closers));
    e->last_bracket = NULL;
    e->citations = (citation_tokens){0};
    e->citation_braces = (citation_brace_index){0};
    e->pending_brackets = NULL;
    e->nonblank_end = 0;
    e->backticks = NULL;
    e->backtick_capacity = 0;
    e->scanned_for_backticks = false;
    e->no_link_openers = true;
    e->oom = 0;
}

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

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
static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(subject *subj, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = subj->pos;
    bufsize_t len = 0;

    while ((c = markdown_core_inline_peek_char(subj)) && (*f)(c)) {
        advance(subj);
        len++;
    }

    return markdown_core_chunk_dup(&subj->input, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
bufsize_t markdown_core_inline_scan_to_closing_backticks(subject *subj, bufsize_t openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array subj->backticks:
        return 0;
    }
    if (!subj->backticks) {
        subj->backtick_capacity = subj->input.len < MAXBACKTICKS ? subj->input.len : MAXBACKTICKS;
        subj->backticks = subj->mem->calloc((size_t)subj->backtick_capacity + 1, sizeof(*subj->backticks));
        if (!subj->backticks) {
            subj->oom = 1;
            return 0;
        }
    }
    if (subj->scanned_for_backticks && subj->backticks[openticklength] <= subj->pos) {
        // return if we already know there's no closer
        return 0;
    }
    while (!found) {
        // read non backticks
        unsigned char c;
        while ((c = markdown_core_inline_peek_char(subj)) && c != '`') {
            advance(subj);
        }
        if (markdown_core_inline_is_eof(subj)) {
            break;
        }
        bufsize_t numticks = 0;
        while (markdown_core_inline_peek_char(subj) == '`') {
            advance(subj);
            numticks++;
        }
        // store position of ender
        if (numticks <= subj->backtick_capacity) {
            subj->backticks[numticks] = subj->pos - numticks;
        }
        if (numticks == openticklength) {
            return (subj->pos);
        }
    }
    // got through whole input without finding closer
    subj->scanned_for_backticks = true;
    return 0;
}

// Destructively modify string, converting newlines to
// spaces, then removing a single leading + trailing space,
// unless the code span consists entirely of space characters.
static void S_normalize_code(markdown_core_strbuf *s) {
    bufsize_t r, w;
    bool contains_nonspace = false;

    for (r = 0, w = 0; r < s->size; ++r) {
        switch (s->ptr[r]) {
        case '\r':
            if (s->ptr[r + 1] != '\n') {
                s->ptr[w++] = ' ';
            }
            break;
        case '\n':
            s->ptr[w++] = ' ';
            break;
        default:
            s->ptr[w++] = s->ptr[r];
        }
        if (s->ptr[r] != ' ') {
            contains_nonspace = true;
        }
    }

    // begins and ends with space?
    if (contains_nonspace && s->ptr[0] == ' ' && s->ptr[w - 1] == ' ') {
        markdown_core_strbuf_drop(s, 1);
        markdown_core_strbuf_truncate(s, w - 2);
    } else {
        markdown_core_strbuf_truncate(s, w);
    }
}

// Parse backtick code section or raw backticks, return an inline.
// Assumes that the subject has a backtick at the current position.
static markdown_core_node *handle_backticks(subject *subj) {
    markdown_core_chunk openticks = take_while(subj, isbacktick);
    bufsize_t startpos = subj->pos;
    bufsize_t endpos = markdown_core_inline_scan_to_closing_backticks(subj, openticks.len);

    if (endpos == 0) {        // not found
        subj->pos = startpos; // rewind
        /* The run stands as its own literal, so it covers ITS OWN BYTES:
         * `startpos` is one past the last of them and the run is
         * `openticks.len` long. Both offsets used to be `subj->pos`, one past
         * the run, so the literal was placed one column right -- and
         * consolidation then carried that end onto the whole merged text run:
         * `hi`lo` reported Text 1:5..1:8 inside a seven-byte paragraph. */
        return make_str(subj, subj->pos - openticks.len, subj->pos - 1, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

        markdown_core_strbuf_set(&buf, subj->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            subj->oom = 1;
        }

        /* A CODE SPAN COVERS ITS BACKTICKS (Q45, answered 2026-08-23). Every
         * other inline construct covers its own delimiters -- emphasis its
         * asterisks, a link its brackets and parens, strikethrough both tilde
         * pairs -- and this one reported the extent of its CONTENT instead,
         * which is what upstream reports and is a defect inherited from it.
         *
         * A scope exists so a consumer can map a node back to the source it
         * came from, and the source a code span came from includes the ticks
         * that make it one. Reporting the content alone also produced a start
         * that is not a place: `` `` `` alone on a line put the span at column
         * 3 of a two-byte line. */
        markdown_core_node *node =
            make_code(subj, startpos - openticks.len, endpos - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        markdown_core_inline_attach_inline_attributes(subj, node, startpos - openticks.len);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

/** Core delimiter rules, keyed by the byte that spells each of them. */
static markdown_core_delimiter_rule delimiter_rule_for_byte(subject *subj, unsigned char c) {
    switch (c) {
    case '*':
        return MARKDOWN_CORE_DELIM_RULE_EMPHASIS;
    case '_':
        return MARKDOWN_CORE_DELIM_RULE_UNDERSCORE;
    default:
        return subj->owner_parser ? subj->owner_parser->delimiter_chars[c] : MARKDOWN_CORE_DELIM_RULE_NONE;
    }
}

static const delimiter_rule_spec CORE_DELIMITER_RULES[MARKDOWN_CORE_DELIM_RULE_COUNT] = {
    [MARKDOWN_CORE_DELIM_RULE_EMPHASIS] = {.minimum_width = 1,
                                           .maximum_width = 2,
                                           .rule_of_three = true,
                                           .body = DELIMITER_INLINE_BODY,
                                           .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                                           .double_kind = MARKDOWN_CORE_NODE_STRONG},
    [MARKDOWN_CORE_DELIM_RULE_UNDERSCORE] = {.minimum_width = 1,
                                             .maximum_width = 2,
                                             .rule_of_three = true,
                                             .body = DELIMITER_INLINE_BODY,
                                             .single_kind = MARKDOWN_CORE_NODE_EMPHASIS,
                                             .double_kind = MARKDOWN_CORE_NODE_STRONG},
};

static const delimiter_rule_spec *delimiter_spec(subject *subj, markdown_core_delimiter_rule rule) {
    const markdown_core_extension *owner = subj->owner_parser ? subj->owner_parser->delimiter_owners[rule] : NULL;
    return owner ? &owner->delimiter : &CORE_DELIMITER_RULES[rule];
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
    if (c == '_') {
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

void markdown_core_inline_pop_bracket(subject *subj) {
    bracket *b;
    if (subj->last_bracket == NULL) {
        return;
    }
    b = subj->last_bracket;
    subj->last_bracket = subj->last_bracket->previous;
    if (b->close_text) {
        if (b->pending_previous) {
            b->pending_previous->pending_next = b->pending_next;
        } else {
            subj->pending_brackets = b->pending_next;
        }
        if (b->pending_next) {
            b->pending_next->pending_previous = b->pending_previous;
        }
        markdown_core_inline_remove_delimiter(subj, b->delim_end);
    }
    markdown_core_inline_free_citation_tokens(subj, &b->citations);
    subj->mem->free(b);
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

static void push_delimiter_boundary(subject *subj, bufsize_t position) {
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

void markdown_core_inline_push_bracket(subject *subj, bracket_kind kind, markdown_core_node *inl_text) {
    bracket *b = (bracket *)subj->mem->calloc(1, sizeof(bracket));
    if (!b) {
        subj->oom = 1;
        return;
    }
    if (subj->last_bracket != NULL) {
        subj->last_bracket->bracket_after = true;
        if (kind != BRACKET_FOOTNOTE) {
            b->in_bracket_image0 = subj->last_bracket->in_bracket_image0;
            b->in_bracket_image1 = subj->last_bracket->in_bracket_image1;
        }
    }
    b->kind = kind;
    markdown_core_citation_open_bracket(subj, b);
    b->outer_no_link_openers = subj->no_link_openers;
    b->active = true;
    b->inl_text = inl_text;
    b->previous = subj->last_bracket;
    b->position = subj->pos;
    b->image_pipe = -1;
    b->bracket_after = false;
    if (kind == BRACKET_IMAGE) {
        b->in_bracket_image1 = true;
    } else if (kind == BRACKET_LINK) {
        b->in_bracket_image0 = true;
    }
    subj->last_bracket = b;
    if (kind != BRACKET_IMAGE) {
        subj->no_link_openers = false;
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

static int extension_dispatches(const markdown_core_extension *ext, unsigned char c) {
    return markdown_core_byte_set_has(ext->dispatch, c);
}

/* Does ANY attached extension claim this byte?
 *
 * That is the only question left for a byte, and it has exactly one caller:
 * `handle_backslash` must not take its run-of-backslashes fast path if an
 * extension might want `\`. This function used to answer "WHICH extension owns
 * a delimiter tagged with this byte", which is a different question with a
 * worse answer -- attach order, or NULL, and NULL is D33. */
static int any_extension_dispatches(markdown_core_parser *parser, unsigned char c) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        if (extension_dispatches((const markdown_core_extension *)tmp_ext->data, c)) {
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

// Parse backslash-escape or just a backslash, returning an inline.
static markdown_core_node *handle_backslash(markdown_core_parser *parser, subject *subj) {
    bufsize_t start = subj->pos;
    /* The line frame BEFORE anything is consumed. The hard-break arm below
     * needs it, and reading it after `markdown_core_inline_skip_line_end` would be right only
     * because `markdown_core_inline_skip_line_end` happens not to advance the frame -- an accident,
     * not a contract. `handle_newline` captures its frame first for the same
     * reason, and the two arms must not merely look symmetric. */
    advance(subj);
    unsigned char nextchar = markdown_core_inline_peek_char(subj);
    if (nextchar == ' ') {
        /* A trailing whitespace run cannot belong to a completed script.
         * Leave it to the inherited text/line-ending scanner, including its
         * trimming and hard-break rules. These lookaheads are disjoint: each
         * begins after its own backslash and ends before the next token. */
        bufsize_t end = subj->pos;
        while (end < subj->input.len && !markdown_core_is_line_end(markdown_core_inline_peek_at(subj, end)) &&
               markdown_core_isspace(markdown_core_inline_peek_at(subj, end))) {
            end++;
            parser->whitespace_work++;
        }
        if ((end == subj->input.len && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(subj->owner->kind)) ||
            (end < subj->input.len && markdown_core_is_line_end(markdown_core_inline_peek_at(subj, end)))) {
            return make_str(subj, start, start, markdown_core_chunk_literal("\\"));
        }
        advance(subj);
        markdown_core_node *escaped = make_str(subj, start, subj->pos - 1, markdown_core_chunk_literal("\\ "));
        if (escaped) {
            /* Contextual escape token: inline completion decodes it once the
             * delimiter/bracket engine has established its semantic owner. */
            escaped->flags |= MARKDOWN_CORE_NODE__ESCAPED_SPACE;
        }
        return escaped;
    }
    if ((parser->backslash_ispunct ? parser->backslash_ispunct : markdown_core_ispunct)(nextchar)) {
        if (nextchar == '\\' && !any_extension_dispatches(parser, '\\')) {
            bufsize_t end = start;
            while (end + 1 < subj->input.len && subj->input.data[end] == '\\' && subj->input.data[end + 1] == '\\') {
                end += 2;
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)subj->mem->calloc((size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    markdown_core_node *run;
                    memset(output, '\\', (size_t)output_len);
                    subj->pos = end;
                    run = make_str(subj, start, end - 1, contents);
                    /* One escape per PAIR: the first backslash of each is the
                     * escape and reaches no literal, the second is the byte the
                     * literal is made of. */
                    for (bufsize_t at = start; run && at + 1 < end; at += 2) {
                    }
                    return run;
                }
            }
        }
        // only ascii symbols and newline can be escaped
        advance(subj);
        {
            markdown_core_node *escaped =
                make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_dup(&subj->input, subj->pos - 1, 1));
            return escaped;
        }
    } else if (!markdown_core_inline_is_eof(subj) && markdown_core_inline_skip_line_end(subj)) {
        push_delimiter_boundary(subj, subj->pos);
        // A backslash hard break CONSUMES a line ending, so the subject has to
        // be told, exactly as handle_newline tells it. It was not, so every node
        // after such a break kept the break's own line and a column measured
        // from the wrong line's start: `foo\` / `bar` reported Text 1:6..1:8 --
        // three columns that do not exist on a four-character line 1.
        // cmark-gfm reports the same numbers, so upstream cannot be the oracle.
        //
        // The node's extent is the bytes that SPELL it: the backslash and the
        // line ending it escapes. The backslash belonged to no node at all
        // before this, so the break is not taking it from anyone.
        markdown_core_node *hard = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (hard) {
            /* The break's extent is the backslash and the line ending it
             * escapes; `subj->pos` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            markdown_core_inline_parser_place(subj, hard, start, subj->pos - 1);
        }
        return hard;
    } else {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("\\"));
    }
}

// Parse an entity or a regular "&" string.
// Assumes the subject has an '&' character at the current position.
static markdown_core_node *handle_entity(subject *subj) {
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT(subj->mem);
    bufsize_t len;

    advance(subj);

    len = houdini_unescape_ent(&ent, subj->input.data + subj->pos, subj->input.len - subj->pos);

    if (len == 0) {
        markdown_core_node *literal = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("&"));
        /* Not an entity: the `&` IS the literal, so it is content. */
        return literal;
    }

    subj->pos += len;
    if (ent.oom) {
        subj->oom = 1;
    }
    return make_str(subj, subj->pos - 1 - len, subj->pos - 1, markdown_core_chunk_buf_detach(&ent));
}

// Parse an autolink, an HTML comment, or an HTML tag.
// Assumes the subject has a '<' character at the current position.
bufsize_t markdown_core_inline_scan_inline_html(subject *subj, bufsize_t pos, unsigned *flags, bool *is_comment) {
    bufsize_t matchlen = 0;
    bool comment = false;
    // finally, try to match an html tag
    if (pos + 2 <= subj->input.len) {
        int c = subj->input.data[pos];
        if (c == '!' && (*flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = subj->input.data[pos + 1];
            if (c == '-' && subj->input.data[pos + 2] == '-') {
                if (subj->input.data[pos + 3] == '>') {
                    matchlen = 4;
                } else if (subj->input.data[pos + 3] == '-' && subj->input.data[pos + 4] == '>') {
                    matchlen = 5;
                } else {
                    matchlen = scan_html_comment(&subj->input, pos + 1);
                    if (matchlen > 0) {
                        matchlen += 1; // prefix "<"
                    } else {           // no match through end of input: set a flag so
                                       // we don't reparse looking for -->:
                        *flags |= FLAG_SKIP_HTML_COMMENT;
                    }
                }
                comment = matchlen > 0;
            } else if (c == '[') {
                if ((*flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(&subj->input, pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (pos + matchlen > subj->input.len) {
                            *flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((*flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(&subj->input, pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (pos + matchlen > subj->input.len) {
                        *flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((*flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(&subj->input, pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (pos + matchlen > subj->input.len) {
                    *flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(&subj->input, pos);
        }
    }
    if (is_comment) {
        *is_comment = comment;
    }
    return matchlen;
}

static markdown_core_node *handle_pointy_brace(subject *subj) {
    bufsize_t matchlen = 0;
    bool comment = false;
    markdown_core_chunk contents;

    advance(subj); // advance past first <

    // first try to match a URL autolink
    matchlen = scan_autolink_uri(&subj->input, subj->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_dup(&subj->input, subj->pos, matchlen - 1);
        subj->pos += matchlen;

        return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 0);
    }

    // next try to match an email autolink
    matchlen = scan_autolink_email(&subj->input, subj->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_dup(&subj->input, subj->pos, matchlen - 1);
        subj->pos += matchlen;

        return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 1);
    }

    matchlen = markdown_core_inline_scan_inline_html(subj, subj->pos, &subj->flags, &comment);
    if (matchlen > 0) {
        if (comment) {
            /* M0: the token is a `Comment` whose literal is the bytes between
             * `<!--` and `-->`. `matchlen` counts from the `!`, so the body
             * starts three bytes past it and the two delimiters take seven of
             * the token's `matchlen + 1` bytes. `<!-->` and `<!--->` are the
             * two tokens the inherited grammar names as comments with nothing
             * inside: their closer overlaps their opener and the literal is
             * empty. The scope is the whole token, delimiters included, as
             * for every raw HTML token. */
            bufsize_t body_len = matchlen > 6 ? matchlen - 6 : 0;
            contents = markdown_core_chunk_dup(&subj->input, subj->pos + 3, body_len);
            subj->pos += matchlen;
            return markdown_core_comment_make_inline(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        }
        contents = markdown_core_chunk_dup(&subj->input, subj->pos - 1, matchlen + 1);
        subj->pos += matchlen;
        markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        return node;
    }

    // if nothing matches, just return the opening <:
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("<"));
}

/* Claim the parsed body of one balanced bracket pair. Span, links/images,
 * and document-owned inline notes share this transfer; their callers decide
 * where the resulting owner lives and when its delimiter boundary closes. */
void markdown_core_inline_take_bracket_content(markdown_core_parser *parser, bracket *opener,
                                               markdown_core_node *owner) {
    markdown_core_node *child = opener->inl_text->next;
    while (child != opener->close_text) {
        markdown_core_node *next = child->next;
        markdown_core_node_unlink(child);
        markdown_core_inline_append_child(owner, child);
        parser->bracket_work++;
        child = next;
    }
}

/* Non-media bracket alternatives own '[' through the suffix. An authored
 * image bang remains an ordinary Text sibling under bracket rules B3-B6. */
void markdown_core_inline_replace_bracket_opener(subject *subj, bracket *opener, markdown_core_node *replacement) {
    if (opener->kind == BRACKET_IMAGE) {
        opener->inl_text->as.literal->len = 1;
        markdown_core_inline_parser_place(subj, opener->inl_text, opener->position - 2, opener->position - 2);
        markdown_core_node_insert_after(opener->inl_text, replacement);
    } else {
        markdown_core_node_insert_before(opener->inl_text, replacement);
        markdown_core_node_free(opener->inl_text);
    }
}

// Return a link, an image, or a literal close bracket.
markdown_core_node *markdown_core_inline_handle_close_bracket(markdown_core_parser *parser, subject *subj) {
    parser->bracket_work++;
    advance(subj);
    bufsize_t initial_pos = subj->pos;
    bracket *opener = subj->last_bracket;
    if (!opener) {
        return make_str(subj, initial_pos - 1, initial_pos - 1, markdown_core_chunk_literal("]"));
    }
    if (opener->kind == BRACKET_FOOTNOTE) {
        return markdown_core_inline_close_inline_footnote(parser, subj, opener);
    }

    /* One bracket owns its parsed children. Alternatives only claim that
     * existing range, in syntax precedence order; none reparses its body. */
    markdown_core_link_candidate link = {0};
    markdown_core_link_match match = markdown_core_link_recognize(subj, opener, &link);
    if (match == LINK_EXPLICIT) {
        if (markdown_core_link_commit(parser, subj, opener, &link, initial_pos)) {
            return NULL;
        }
        goto no_match;
    }
    subj->pos = initial_pos;
    markdown_core_bracket_match span = markdown_core_span_close(parser, subj, opener);
    if (span == BRACKET_MATCHED) {
        return NULL;
    }
    if (span == BRACKET_REJECTED) {
        goto no_match;
    }
    markdown_core_node *close = NULL;
    if (markdown_core_citation_defer_tail(subj, opener, &close)) {
        return close;
    }
    if (markdown_core_inline_close_bibliography(parser, subj, opener)) {
        return NULL;
    }
    if (match == LINK_SHORTCUT) {
        if (markdown_core_link_commit(parser, subj, opener, &link, initial_pos)) {
            return NULL;
        }
        goto no_match;
    }
    if (markdown_core_footnote_close_reference(parser, subj, opener)) {
        return NULL;
    }

no_match:
    markdown_core_inline_finish_citation_tokens(subj, &opener->citations);
    markdown_core_inline_pop_bracket(subj);
    subj->pos = initial_pos;
    return make_str(subj, initial_pos - 1, initial_pos - 1, markdown_core_chunk_literal("]"));
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the subject has a cr or newline at the current position.
// A break node's extent is the line ending it stands for, read in the frame of
// the line being LEFT -- so it is captured here, before `subj->line` and
// `subj->column_offset` move on to the next one. That column is one past the
// last byte of its line, which is where a line ending is; Q40 says so and
// `scripts/audit-position-places.mjs` admits it for break nodes and for nothing
// else. Before this the node was `calloc`'d and never written, so 153 golden
// rows across seven files said `0:0..0:0` -- a coordinate that is not a place
// either, and one that carries no line at all.
static markdown_core_node *handle_newline(subject *subj) {
    push_delimiter_boundary(subj, subj->pos + 1);
    bufsize_t nlpos = subj->pos;
    markdown_core_node *brk;
    // skip over cr, crlf, or lf:
    if (markdown_core_inline_peek_at(subj, subj->pos) == '\r') {
        advance(subj);
    }
    if (markdown_core_inline_peek_at(subj, subj->pos) == '\n') {
        advance(subj);
    }
    // skip spaces at beginning of line
    markdown_core_inline_skip_spaces(subj);
    if (nlpos > 1 && markdown_core_inline_peek_at(subj, nlpos - 1) == ' ' &&
        markdown_core_inline_peek_at(subj, nlpos - 2) == ' ') {
        brk = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        brk = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_SOFT_BREAK);
    }
    if (brk) {
        // The two spaces of a hard break stay with the text they follow, as
        // upstream also has them, so both forms own exactly the line ending.
        markdown_core_inline_parser_place(subj, brk, nlpos, nlpos);
        /* The break is the line ending. The spaces skipped after it are the
         * next line's leading whitespace: the parse read them and kept them
         * nowhere, which is what DISCARDED is for, and they belong to the
         * block they were read inside rather than to the break. */
    }
    return brk;
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
        } else if (n > subj->pos && true) {
            return n;
        } else {
            n++;
        }
    }
    return subj->input.len;
}

static int is_core_special_character(unsigned char c) {
    switch (c) {
    case '\r':
    case '\n':
    case '\\':
    case '`':
    case '&':
    case '_':
    case '*':
    case '[':
    case ']':
    case '<':
    case '!':
        return 1;
    default:
        return 0;
    }
}

void markdown_core_inlines_reset_special_chars(markdown_core_parser *parser) {
    memcpy(parser->special_chars, BASE_SPECIAL_CHARS, sizeof(parser->special_chars));
    memcpy(parser->skip_chars, BASE_SKIP_CHARS, sizeof(parser->skip_chars));
}

void markdown_core_inlines_add_text_terminator(markdown_core_parser *parser, unsigned char c) {
    if (is_core_special_character(c)) {
        return;
    }
    parser->special_chars[c] = 1;
}

void markdown_core_inlines_remove_text_terminator(markdown_core_parser *parser, unsigned char c) {
    if (is_core_special_character(c)) {
        return;
    }
    parser->special_chars[c] = 0;
}

void markdown_core_inlines_add_flanking_transparent(markdown_core_parser *parser, unsigned char c) {
    if (is_core_special_character(c)) {
        return;
    }
    parser->skip_chars[c] = 1;
}

void markdown_core_inlines_remove_flanking_transparent(markdown_core_parser *parser, unsigned char c) {
    if (is_core_special_character(c)) {
        return;
    }
    parser->skip_chars[c] = 0;
}

static markdown_core_node *try_extensions(markdown_core_parser *parser, markdown_core_node *parent, unsigned char c,
                                          subject *subj) {
    markdown_core_node *res = NULL;
    markdown_core_llist *tmp;

    for (tmp = parser->inline_extensions; tmp; tmp = tmp->next) {
        const markdown_core_extension *ext = (const markdown_core_extension *)tmp->data;

        if (!extension_dispatches(ext, c)) {
            continue;
        }

        res = ext->match_inline(ext, parser, parent, c, subj);

        if (res) {
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
    markdown_core_chunk contents;
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
    if (markdown_core_heading_claim_tail(subj, parent)) {
        return 0;
    }
    switch (c) {
    case '\r':
    case '\n':
        /* A break's bytes reach no literal: a `SoftBreak` and a `LineBreak`
         * both have none, and a hard break's two trailing spaces or backslash
         * are exactly the syntax that made it one. */
        new_inl = handle_newline(subj);
        break;
    case '`':
        new_inl = handle_backticks(subj);
        break;
    case '\\':
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl == NULL && !parser->oom && !subj->oom) {
            new_inl = handle_backslash(parser, subj);
        }
        break;
    case '&':
        /* `&amp;` reaches the literal as `&`, and that `&` is not the `&` the
         * source wrote -- no byte of the entity survives as itself. An `&`
         * that is NOT an entity is returned by the same handler as its own
         * literal, and claims itself CONTENT. */
        new_inl = handle_entity(subj);
        break;
    case '<':
        new_inl = handle_pointy_brace(subj);
        break;
    case '*':
    case '_': {
        const delimiter_run *run = scan_delimiter(subj, subj->pos, delimiter_rule_for_byte(subj, c));
        if (!delimiter_needs_stack(subj, run)) {
            goto text;
        }
        /* A `*`, `_`, `=`, or `+` run is CONTENT until it matches -- an unmatched one IS
         * its own literal -- and `S_insert_delimited_inline` re-claims the bytes it uses.
         * Quotation marks, hyphens, and periods are not here: the dialect has
         * no smart punctuation, so they are ordinary text stored as written,
         * and the text arm below owns them like any other byte. */
        new_inl = handle_delim(subj, run);
        break;
    }
    case '[':
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl != NULL || parser->oom || subj->oom) {
            break;
        }
        advance(subj);
        /* CONTENT until it matches: an unmatched `[` IS its own literal, and
         * `markdown_core_inline_handle_close_bracket` re-claims it MARKER for the link it opens. */
        new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("["));
        if (new_inl) {
            markdown_core_inline_push_bracket(subj, BRACKET_LINK, new_inl);
        }
        break;
    case ']':
        /* Opaque tokens consume their brackets before this dispatch. Every
         * remaining close bracket belongs to the shared bracket procedure;
         * no extension dispatches on ']'. Never rescan the delimiter stack. */
        new_inl = markdown_core_inline_handle_close_bracket(parser, subj);
        break;
    case '!':
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl != NULL || parser->oom || subj->oom) {
            break;
        }

        advance(subj);
        if (markdown_core_inline_peek_char(subj) == '[' && markdown_core_inline_peek_char_n(subj, 1) != '^') {
            advance(subj);
            new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_literal("!["));
            if (new_inl) {
                markdown_core_inline_push_bracket(subj, BRACKET_IMAGE, new_inl);
            }
        } else {
            new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("!"));
        }
        break;
    default:
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl != NULL || parser->oom || subj->oom) {
            break;
        }

    text:
        endpos = subject_find_special_char(subj);
        if (subj->pos < subj->heading_content_end && endpos > subj->heading_content_end) {
            endpos = subj->heading_content_end;
        }
        /* Disjoint ordinary text slices alone contribute whitespace barriers.
         * Escapes, entities, and opaque tokens have their own token owners. */
        for (bufsize_t at = subj->pos; at < endpos;) {
            int32_t scalar = 0;
            int width = markdown_core_utf8proc_iterate(subj->input.data + at, endpos - at, &scalar);
            parser->whitespace_work++;
            if (markdown_core_utf8proc_is_space(scalar)) {
                push_delimiter_boundary(subj, at + width);
            }
            at += width > 0 ? width : 1;
        }
        /* Text runs are disjoint, so recording separators costs at most one
         * extra visit per byte, regardless of bracket nesting or digit-run
         * length. No image closer scans its label again. */
        markdown_core_media_record_text(parser, subj, endpos);
        contents = markdown_core_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
        startpos = subj->pos;
        subj->pos = endpos;

        // if we're at a newline, strip trailing spaces.
        if (markdown_core_is_line_end(markdown_core_inline_peek_char(subj))) {
            markdown_core_chunk_rtrim(&contents);
        }

        new_inl = make_str(subj, startpos, endpos - 1, contents);
        /* `rtrim` above takes the trailing spaces before a line ending OUT of
         * the literal, and the run's SCOPE still covers them -- so they are the
         * run's, in the role a byte kept nowhere has. Giving them to the block
         * instead left the node covering eight columns and owning three, which
         * is what L5 measures. */
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

    markdown_core_heading_begin_inlines(parser, subj, parent);
}

void markdown_core_inline_clear_inlines(subject *subj) {
    markdown_core_parser *parser = subj->owner_parser;
    markdown_core_inline_free_citation_tokens(subj, &subj->citations);
    subj->mem->free(subj->citation_braces.entries);
    subj->citation_braces = (citation_brace_index){0};
    subj->mem->free(subj->backticks);
    subj->backticks = NULL;
    // free bracket and delim stack
    while (subj->last_delim) {
        markdown_core_inline_remove_delimiter(subj, subj->last_delim);
    }
    while (subj->last_bracket) {
        markdown_core_inline_pop_bracket(subj);
    }
    while (subj->pending_brackets) {
        bracket *next = subj->pending_brackets->pending_next;
        markdown_core_inline_free_citation_tokens(subj, &subj->pending_brackets->citations);
        subj->mem->free(subj->pending_brackets);
        subj->pending_brackets = next;
    }

    if (subj->attributes.mem) {
        parser->attribute_work += subj->attributes.work;
        markdown_core_attribute_parser_free(&subj->attributes);
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
        for (bracket *open = subj->last_bracket; open; open = open->previous) {
            markdown_core_inline_finish_citation_tokens(subj, &open->citations);
        }
        markdown_core_inline_finish_citation_tokens(subj, &subj->citations);
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

int markdown_core_inline_parser_in_bracket(markdown_core_inline_parser *parser, int image) {
    bracket *b = parser->last_bracket;
    if (!b) {
        return 0;
    }
    if (image != 0) {
        return b->in_bracket_image1;
    } else {
        return b->in_bracket_image0;
    }
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
unsigned char markdown_core_inline_parser_closing_bracket(markdown_core_inline_parser *parser) {
    return parser->last_bracket ? ']' : 0;
}

int markdown_core_inline_parser_context_start(markdown_core_inline_parser *parser) {
    bracket *opener = parser->last_bracket;
    return opener && opener->kind == BRACKET_FOOTNOTE ? opener->position : 0;
}

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
