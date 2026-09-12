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
#define make_str(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)
#define make_code(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_CODE, sc, ec, s)
#define make_raw_html(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_HTML, sc, ec, s)
#define make_comment(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_COMMENT, sc, ec, s)
#define make_line_break(mem) make_simple(mem, MARKDOWN_CORE_NODE_LINE_BREAK)
#define make_soft_break(mem) make_simple(mem, MARKDOWN_CORE_NODE_SOFT_BREAK)

#define MAXBACKTICKS 80

/* Citation candidates borrow token nodes until the enclosing bracket chooses
 * their owner. They retain source coordinates, never a second inline tree. */
typedef struct citation_token {
    struct citation_token *next;
    markdown_core_node *node;
    delimiter *boundary;
    struct bracket *tail;
    bufsize_t start, key_start, key_end, end, tail_start;
    bool key, suppress;
} citation_token;

typedef struct {
    citation_token *first, *last;
} citation_tokens;

typedef struct {
    bufsize_t start, end, previous;
    bool valid, content;
} citation_brace;

typedef struct {
    citation_brace *entries;
    size_t count, cursor;
    bool ready;
} citation_brace_index;

typedef enum { BRACKET_LINK, BRACKET_IMAGE, BRACKET_FOOTNOTE } bracket_kind;

typedef struct bracket {
    struct bracket *previous;
    markdown_core_node *inl_text;
    bufsize_t position;
    /* Last pipe read as ordinary text at this bracket's depth. Opaque tokens,
     * escapes and nested brackets never update the enclosing image. */
    bufsize_t image_pipe;
    bracket_kind kind;
    bool outer_no_link_openers;
    bool active;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
    citation_tokens citations;
    citation_token *author;
    /* A tail waits for its key's enclosing owner. All token nodes stay in the
     * AST; this parser-owned continuation borrows the exact bounded range. */
    markdown_core_node *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    markdown_core_map_record *pending_reference;
    struct bracket *pending_previous, *pending_next;
} bracket;

#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)

/* One maximal core run, classified from immutable source bytes. The text
 * scanner may retain one lookahead run for delimiter dispatch to consume. */
typedef struct {
    bufsize_t start, end;
    markdown_core_delimiter_rule rule;
    bool can_open, can_close;
} core_delimiter_run;

typedef struct subject {
    markdown_core_mem *mem;
    markdown_core_chunk input;
    markdown_core_attribute_parser attributes;
    bufsize_t heading_attributes_start, heading_content_end, heading_label_end;
    unsigned flags;
    bufsize_t opaque_end;
    /* One plus the start of a suffix proven to contain no closer of a rule. */
    bufsize_t opaque_failed_from[MARKDOWN_CORE_DELIM_RULE_COUNT];
    int line;
    bufsize_t pos;
    /* The block whose content buffer `input` is, and the parser that holds
     * requirement 10's content-to-source map for it. Both are NULL for a
     * subject built straight out of a chunk -- the reference-definition
     * parser -- and the map is then simply not consulted. */
    markdown_core_parser *owner_parser;
    markdown_core_node *owner;
    markdown_core_map *refmap;
    delimiter *last_delim;
    core_delimiter_run core_run;
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
    /* Borrowed from the owning parser (or the immutable core defaults when
     * there is no parser, e.g. reference parsing). */
    const int8_t *special_chars;
    const int8_t *skip_chars;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    int oom;
} subject;

// "\r\n\\`&_*+=[]<!"
static const int8_t BASE_SPECIAL_CHARS[256] = {
    ['\n'] = 1, ['\r'] = 1, ['\\'] = 1, ['`'] = 1, ['&'] = 1, ['_'] = 1, ['*'] = 1, ['='] = 1, ['+'] = 1,
    ['['] = 1,  [']'] = 1,  ['<'] = 1,  ['!'] = 1, ['^'] = 1, ['~'] = 1, ['@'] = 1, ['-'] = 1, [';'] = 1,
};

// No emphasis-boundary skip characters by default; attached inline extensions
// add theirs to the parser-local copy.
static const int8_t BASE_SKIP_CHARS[256] = {0};

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static delimiter *S_insert_delimited_inline(subject *subj, delimiter *opener, delimiter *closer, bufsize_t use_delims,
                                            markdown_core_node_type kind);

static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent);
static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, subject *subj);
static void resolve_citation_tail(subject *subj, citation_token *token, bool ordinary);

static void subject_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number, subject *e,
                             markdown_core_chunk *buffer, markdown_core_map *refmap);
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
static MARKDOWN_CORE_INLINE void S_place_inline(subject *subj, markdown_core_node *node, int from, int to) {
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

void markdown_core_inline_parser_place(markdown_core_inline_parser *parser, markdown_core_node *node, int from,
                                       int to) {
    S_place_inline(parser, node, from, to);
}

int markdown_core_inline_parser_attributes(markdown_core_inline_parser *parser, bufsize_t start,
                                           markdown_core_attributes *value, bufsize_t *end) {
    if (start == parser->heading_attributes_start) {
        return 0;
    }
    if (!parser->attributes.mem) {
        parser->attributes.mem = parser->mem;
        parser->attributes.data = parser->input.data;
        parser->attributes.length = parser->input.len;
    }
    int matched = markdown_core_attributes_parse(&parser->attributes, start, value, end);
    if (parser->attributes.oom) {
        parser->oom = 1;
    }
    return matched;
}

/* Inline owners consume one immediate suffix. A heading's trailing container
 * belongs to its block envelope, and is reserved until the inline cursor
 * proves it is outside an opaque body. */
static void attach_inline_attributes(subject *subj, markdown_core_node *node, bufsize_t from) {
    bufsize_t end;
    if (markdown_core_inline_parser_attributes(subj, subj->pos, &node->attributes, &end)) {
        subj->pos = end;
        S_place_inline(subj, node, from, end - 1);
    }
}

// Create an inline with a literal string value.
static MARKDOWN_CORE_INLINE markdown_core_node *make_literal(subject *subj, markdown_core_node_type t, int start_column,
                                                             int end_column, markdown_core_chunk s) {
    markdown_core_node *e = markdown_core_node_new_with_mem(t, subj->mem);
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(subj->mem, &s);
        subj->oom = 1;
        return NULL;
    }
    *e->as.literal = s;
    S_place_inline(subj, e, start_column, end_column);
    return e;
}

// Create an inline with no value.
static MARKDOWN_CORE_INLINE markdown_core_node *make_simple(markdown_core_mem *mem, markdown_core_node_type t) {
    return markdown_core_node_new_with_mem(t, mem);
}

/* make_simple with the subject's loss flag for handlers that consume input
 * before creating the node. */
static MARKDOWN_CORE_INLINE markdown_core_node *make_simple_subj(subject *subj, markdown_core_node_type t) {
    markdown_core_node *e = make_simple(subj->mem, t);
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
static void append_child(markdown_core_node *node, markdown_core_node *child) {
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
    markdown_core_node *link = make_simple(subj->mem, MARKDOWN_CORE_NODE_LINK);
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
    S_place_inline(subj, link, start_column, end_column);
    text = make_str_with_entities(subj, start_column + 1, end_column - 1, &url);
    if (text) {
        append_child(link, text);
    }
    attach_inline_attributes(subj, link, start_column);
    /* The pointy braces are the syntax; what they enclose is the text. */
    return link;
}

static void subject_from_buf(markdown_core_parser *parser, markdown_core_mem *mem, int line_number, subject *e,
                             markdown_core_chunk *chunk, markdown_core_map *refmap) {
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
    e->core_run = (core_delimiter_run){0};
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

static MARKDOWN_CORE_INLINE unsigned char peek_char_n(subject *subj, bufsize_t n) {
    // NULL bytes should have been stripped out by now.  If they're
    // present, it's a programming error:
    assert(!(subj->pos + n < subj->input.len && subj->input.data[subj->pos + n] == 0));
    return (subj->pos + n < subj->input.len) ? subj->input.data[subj->pos + n] : 0;
}

static MARKDOWN_CORE_INLINE unsigned char peek_char(subject *subj) { return peek_char_n(subj, 0); }

static MARKDOWN_CORE_INLINE unsigned char peek_at(subject *subj, bufsize_t pos) { return subj->input.data[pos]; }

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
    return subj->skip_chars[peek_at(subj, pos)];
}

// Return true if there are more characters in the subject.
static MARKDOWN_CORE_INLINE int is_eof(subject *subj) { return (subj->pos >= subj->input.len); }

// Advance the subject.  Doesn't check for eof.
#define advance(subj) (subj)->pos += 1

static MARKDOWN_CORE_INLINE bool skip_spaces(subject *subj) {
    bool skipped = false;
    while (peek_char(subj) == ' ' || peek_char(subj) == '\t') {
        advance(subj);
        skipped = true;
    }
    return skipped;
}

static MARKDOWN_CORE_INLINE bool skip_line_end(subject *subj) {
    bool seen_line_end_char = false;
    if (peek_char(subj) == '\r') {
        advance(subj);
        seen_line_end_char = true;
    }
    if (peek_char(subj) == '\n') {
        advance(subj);
        seen_line_end_char = true;
    }
    return seen_line_end_char || is_eof(subj);
}

// Take characters while a predicate holds, and return a string.
static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(subject *subj, int (*f)(int)) {
    unsigned char c;
    bufsize_t startpos = subj->pos;
    bufsize_t len = 0;

    while ((c = peek_char(subj)) && (*f)(c)) {
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
static bufsize_t scan_to_closing_backticks(subject *subj, bufsize_t openticklength) {

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
        while ((c = peek_char(subj)) && c != '`') {
            advance(subj);
        }
        if (is_eof(subj)) {
            break;
        }
        bufsize_t numticks = 0;
        while (peek_char(subj) == '`') {
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
    bufsize_t endpos = scan_to_closing_backticks(subj, openticks.len);

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
        attach_inline_attributes(subj, node, startpos - openticks.len);
        /* The ticks reach no literal and the bytes between them do. */
        return node;
    }
}

/** Core delimiter rules, keyed by the byte that spells each of them. */
static markdown_core_delimiter_rule core_delimiter_rule(unsigned char c) {
    switch (c) {
    case '*':
        return MARKDOWN_CORE_DELIM_RULE_EMPHASIS;
    case '_':
        return MARKDOWN_CORE_DELIM_RULE_UNDERSCORE;
    case '=':
        return MARKDOWN_CORE_DELIM_RULE_MARK;
    case '+':
        return MARKDOWN_CORE_DELIM_RULE_INSERTION;
    case '^':
        return MARKDOWN_CORE_DELIM_RULE_SUPERSCRIPT;
    default:
        return MARKDOWN_CORE_DELIM_RULE_NONE;
    }
}

/* Run spelling, pair ambiguity and body grammar are independent rule
 * properties. Every parsed body uses the same matcher and constructor;
 * opaque bodies retain their extension's literal decoder. */
typedef enum { DELIMITER_INLINE_BODY, DELIMITER_WORD_BODY } delimiter_body;
typedef struct {
    bufsize_t minimum_width, maximum_width;
    bufsize_t run_limit; /* zero means a maximal run */
    bool rule_of_three;
    delimiter_body body;
    markdown_core_node_type single_kind, double_kind;
} delimiter_rule_spec;

static const delimiter_rule_spec DELIMITER_RULES[MARKDOWN_CORE_DELIM_RULE_COUNT] = {
    [MARKDOWN_CORE_DELIM_RULE_EMPHASIS] = {1, 2, 0, true, DELIMITER_INLINE_BODY, MARKDOWN_CORE_NODE_EMPHASIS,
                                           MARKDOWN_CORE_NODE_STRONG},
    [MARKDOWN_CORE_DELIM_RULE_UNDERSCORE] = {1, 2, 0, true, DELIMITER_INLINE_BODY, MARKDOWN_CORE_NODE_EMPHASIS,
                                             MARKDOWN_CORE_NODE_STRONG},
    [MARKDOWN_CORE_DELIM_RULE_MARK] = {2, 2, 0, false, DELIMITER_INLINE_BODY, MARKDOWN_CORE_NODE_NONE,
                                       MARKDOWN_CORE_NODE_MARK},
    [MARKDOWN_CORE_DELIM_RULE_INSERTION] = {2, 2, 0, false, DELIMITER_INLINE_BODY, MARKDOWN_CORE_NODE_NONE,
                                            MARKDOWN_CORE_NODE_INSERTION},
    [MARKDOWN_CORE_DELIM_RULE_SUPERSCRIPT] = {1, 1, 1, false, DELIMITER_WORD_BODY, MARKDOWN_CORE_NODE_SUPERSCRIPT,
                                              MARKDOWN_CORE_NODE_NONE},
    [MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT] = {1, 1, 1, false, DELIMITER_WORD_BODY, MARKDOWN_CORE_NODE_SUBSCRIPT,
                                            MARKDOWN_CORE_NODE_NONE},
    [MARKDOWN_CORE_DELIM_RULE_STRIKETHROUGH] = {2, 2, 0, false, DELIMITER_INLINE_BODY, MARKDOWN_CORE_NODE_NONE,
                                                MARKDOWN_CORE_NODE_STRIKETHROUGH},
};

/* Classify without moving the parser cursor or allocating an AST node.
 * A cached lookahead is keyed by its source offset, so text scanning and
 * delimiter dispatch consume the same classification even after a rewind. */
static const core_delimiter_run *scan_core_delimiter(subject *subj, bufsize_t start) {
    if (subj->core_run.rule != MARKDOWN_CORE_DELIM_RULE_NONE && subj->core_run.start == start) {
        return &subj->core_run;
    }
    unsigned char c = peek_at(subj, start);
    core_delimiter_run run = {.start = start, .end = start, .rule = core_delimiter_rule(c)};
    assert(run.rule != MARKDOWN_CORE_DELIM_RULE_NONE);
    const delimiter_rule_spec *spec = &DELIMITER_RULES[run.rule];
    while (run.end < subj->input.len && peek_at(subj, run.end) == c &&
           (!spec->run_limit || run.end - run.start < spec->run_limit)) {
        run.end++;
        if (subj->owner_parser) {
            subj->owner_parser->delimiter_work++;
        }
    }
    if (spec->body == DELIMITER_WORD_BODY) {
        run.can_open = run.can_close = true;
        subj->core_run = run;
        return &subj->core_run;
    }
    if (run.end - run.start < DELIMITER_RULES[run.rule].minimum_width) {
        subj->core_run = run;
        return &subj->core_run;
    }

    bufsize_t before_char_pos, after_char_pos;
    int32_t after_char = 0, before_char = 0;
    int len;
    if (run.start == 0) {
        before_char = 10;
    } else {
        before_char_pos = run.start - 1;
        // Walk back to the beginning of the UTF-8 sequence.
        while ((peek_at(subj, before_char_pos) >> 6 == 2 || subj->skip_chars[peek_at(subj, before_char_pos)]) &&
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
    subj->core_run = run;
    return &subj->core_run;
}

/* Source classification is immutable, but eligibility depends on the live
 * stack. A close-only run cannot match a future opener. Keep it as text when
 * no earlier opener of its rule survives; runs that can open must remain
 * eligible even without an earlier opener. Counts are conservative because
 * pair reduction is deferred and one run can supply several delimiter units. */
static bool core_delimiter_needs_stack(const subject *subj, const core_delimiter_run *run) {
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

static void remove_delimiter(subject *subj, delimiter *delim) {
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

static void free_citation_tokens(subject *subj, citation_tokens *tokens) {
    while (tokens->first) {
        citation_token *next = tokens->first->next;
        subj->mem->free(tokens->first);
        tokens->first = next;
    }
    tokens->last = NULL;
}

static void pop_bracket(subject *subj) {
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
        remove_delimiter(subj, b->delim_end);
    }
    free_citation_tokens(subj, &b->citations);
    subj->mem->free(b);
}

static delimiter *push_delimiter_entry(subject *subj, delimiter_kind kind, bufsize_t position) {
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
        push_delimiter_entry(subj, DELIMITER_BOUNDARY, position);
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
            remove_delimiter(subj, entry);
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
            remove_delimiter(subj, entry->previous);
        }
    } else {
        remove_delimiter(subj, entry);
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
    delim = push_delimiter_entry(subj, DELIMITER_MARKER, subj->pos);
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

static bufsize_t scan_inline_html(subject *subj, bufsize_t pos, unsigned *flags, bool *is_comment);

static bool citation_key_char(int32_t scalar) {
    return scalar == '_' || markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar);
}

static bool citation_opener(subject *subj, bufsize_t pos) {
    if (!pos) {
        return true;
    }
    bufsize_t before = pos - 1;
    while (before && (subj->input.data[before] & 0xc0) == 0x80) {
        before--;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(subj->input.data + before, pos - before, &scalar);
    return !citation_key_char(scalar);
}

/* The text lexer only needs the key's first scalar to reject a literal @.
 * Full keys, including balanced braces, are consumed by scan_citation_key. */
static bool citation_key_follows(subject *subj, bufsize_t at) {
    if (at >= subj->input.len) {
        return false;
    }
    if (subj->input.data[at] == '{') {
        return true;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(subj->input.data + at, subj->input.len - at, &scalar);
    return citation_key_char(scalar);
}

static bool source_escaped(subject *subj, bufsize_t at, bufsize_t begin) {
    bufsize_t escape = at;
    while (escape > begin && subj->input.data[escape - 1] == '\\') {
        escape--;
        subj->owner_parser->citation_work++;
    }
    return (at - escape) % 2 != 0;
}

/* Balanced braced keys share one lexical index per input extent. A failed
 * outer candidate leaves every inner key available without rescanning its
 * suffix. Only opening brace events allocate records; their previous indices
 * form the balancing stack. Opaque code/tag tokens contribute bytes but no
 * brace events. Queries follow the inline token cursor in source order. */
static void prepare_citation_braces(subject *subj) {
    citation_brace_index *index = &subj->citation_braces;
    index->ready = true;
    size_t capacity = 0;
    bufsize_t top = -1;
    unsigned html_flags = 0;
    for (bufsize_t at = 0; at < subj->input.len;) {
        bufsize_t opaque_end = at;
        unsigned char c = subj->input.data[at];
        bool escaped = (c == '`' || c == '<') && source_escaped(subj, at, 0);
        if (c == '`' && !escaped) {
            bufsize_t run = at;
            while (run < subj->input.len && subj->input.data[run] == '`') {
                run++;
            }
            bufsize_t saved = subj->pos;
            subj->pos = run;
            opaque_end = scan_to_closing_backticks(subj, run - at);
            subj->pos = saved;
            if (!opaque_end) {
                opaque_end = run;
            }
        } else if (c == '<' && !escaped) {
            bufsize_t width = scan_inline_html(subj, at + 1, &html_flags, NULL);
            if (!width) {
                width = scan_autolink_uri(&subj->input, at + 1);
            }
            if (!width) {
                width = scan_autolink_email(&subj->input, at + 1);
            }
            if (width) {
                opaque_end = at + 1 + width;
            }
        }
        if (opaque_end > at) {
            while (at < opaque_end) {
                int32_t scalar;
                int width = markdown_core_utf8proc_iterate(subj->input.data + at, opaque_end - at, &scalar);
                subj->owner_parser->citation_work++;
                if (top >= 0) {
                    index->entries[top].content = true;
                    if (markdown_core_utf8proc_is_space(scalar)) {
                        index->entries[top].valid = false;
                    }
                }
                at += width > 0 ? width : 1;
            }
            continue;
        }
        subj->owner_parser->citation_work++;
        if (c == '{') {
            if (index->count == capacity) {
                if (capacity > SIZE_MAX / sizeof(*index->entries) / 2) {
                    subj->oom = 1;
                    return;
                }
                size_t grown = capacity ? capacity * 2 : 8;
                void *entries = subj->mem->realloc(index->entries, grown * sizeof(*index->entries));
                if (!entries) {
                    subj->oom = 1;
                    return;
                }
                index->entries = entries;
                subj->owner_parser->citation_brace_bytes += (grown - capacity) * sizeof(*index->entries);
                capacity = grown;
            }
            index->entries[index->count] = (citation_brace){.start = at++, .previous = top, .valid = true};
            top = (bufsize_t)index->count++;
        } else if (c == '}' && top >= 0) {
            citation_brace *brace = &index->entries[top];
            bool valid = brace->valid && brace->content;
            brace->end = valid ? at + 1 : 0;
            top = brace->previous;
            if (top >= 0) {
                index->entries[top].valid &= valid;
                index->entries[top].content = true;
            }
            at++;
        } else {
            int32_t scalar;
            int width = markdown_core_utf8proc_iterate(subj->input.data + at, subj->input.len - at, &scalar);
            if (top >= 0) {
                index->entries[top].content = true;
                if (markdown_core_utf8proc_is_space(scalar)) {
                    index->entries[top].valid = false;
                }
            }
            at += width > 0 ? width : 1;
        }
    }
}

static bool scan_citation_key(subject *subj, bufsize_t start, citation_token *token) {
    bufsize_t pos = start;
    *token = (citation_token){.start = start, .key = true};
    if (!citation_opener(subj, start)) {
        return false;
    }
    if (peek_at(subj, pos) == '-') {
        token->suppress = true;
        pos++;
    }
    if (peek_at(subj, pos) != '@') {
        return false;
    }
    pos++;
    if (peek_at(subj, pos) == '{') {
        citation_brace_index *index = &subj->citation_braces;
        if (!index->ready) {
            prepare_citation_braces(subj);
        }
        if (subj->oom) {
            return false;
        }
        while (index->cursor < index->count && index->entries[index->cursor].start < pos) {
            subj->owner_parser->citation_work++;
            index->cursor++;
        }
        if (index->cursor == index->count || index->entries[index->cursor].start != pos ||
            !index->entries[index->cursor].end) {
            return false;
        }
        token->key_start = pos + 1;
        token->end = index->entries[index->cursor].end;
        token->key_end = token->end - 1;
        return true;
    }
    token->key_start = pos;
    while (pos < subj->input.len) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(subj->input.data + pos, subj->input.len - pos, &scalar);
        subj->owner_parser->citation_work++;
        if (citation_key_char(scalar)) {
            pos += width;
        } else if (pos > token->key_start && scalar < 128 && strchr(":.#$%&-+?<>~/", scalar) &&
                   pos + width < subj->input.len) {
            int32_t next;
            markdown_core_utf8proc_iterate(subj->input.data + pos + width, subj->input.len - pos - width, &next);
            if (!citation_key_char(next)) {
                break;
            }
            pos += width;
        } else {
            break;
        }
    }
    token->key_end = token->end = pos;
    return pos > token->key_start;
}

static citation_tokens *current_citation_tokens(subject *subj) {
    return subj->last_bracket ? &subj->last_bracket->citations : &subj->citations;
}

static markdown_core_node *read_citation_token(subject *subj, bool key) {
    citation_token value = {.start = subj->pos, .end = subj->pos + 1};
    if (key && !scan_citation_key(subj, subj->pos, &value)) {
        return NULL;
    }
    markdown_core_node *text = make_str(subj, value.start, value.end - 1,
                                        markdown_core_chunk_dup(&subj->input, value.start, value.end - value.start));
    if (!text) {
        return NULL;
    }
    citation_token *token = subj->mem->calloc(1, sizeof(*token));
    delimiter *boundary = token ? push_delimiter_entry(subj, DELIMITER_CITATION_TOKEN, value.end) : NULL;
    if (!boundary) {
        subj->mem->free(token);
        markdown_core_node_free(text);
        subj->oom = 1;
        return NULL;
    }
    *token = value;
    token->tail_start = -1;
    if (key) {
        bufsize_t at = value.end;
        unsigned lines = 0;
        while (at < subj->input.len && (peek_at(subj, at) == ' ' || peek_at(subj, at) == '\t' ||
                                        peek_at(subj, at) == '\n' || peek_at(subj, at) == '\r')) {
            subj->owner_parser->citation_work++;
            if (peek_at(subj, at) == '\n') {
                lines++;
            }
            at++;
        }
        if (lines <= 1 && peek_at(subj, at) == '[' && peek_at(subj, at + 1) != '^') {
            token->tail_start = at;
        }
    }
    token->node = text;
    token->boundary = boundary;
    citation_tokens *tokens = current_citation_tokens(subj);
    if (tokens->last) {
        tokens->last->next = token;
    } else {
        tokens->first = token;
    }
    tokens->last = token;
    subj->pos = value.end;
    return text;
}

static void push_bracket(subject *subj, bracket_kind kind, markdown_core_node *inl_text) {
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
    citation_token *author = current_citation_tokens(subj)->last;
    if (kind == BRACKET_LINK && author && author->key && author->tail_start == subj->pos - 1) {
        b->author = author;
    }
    b->kind = kind;
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

static markdown_core_node *handle_delim(subject *subj, const core_delimiter_run *run) {
    assert(core_delimiter_needs_stack(subj, run));
    subj->pos = run->end;
    markdown_core_node *inl_text = make_str(subj, run->start, run->end - 1,
                                            markdown_core_chunk_dup(&subj->input, run->start, run->end - run->start));
    // One eligible maximal run owns one stack entry and cannot match itself.
    if (inl_text) {
        push_delimiter(subj, NULL, run->rule, run->can_open, run->can_close, inl_text);
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

static void process_delimiters(markdown_core_parser *parser, subject *subj, bufsize_t stack_bottom, delimiter *after) {
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
                if (closer->kind == DELIMITER_AFFIX_BOUNDARY || DELIMITER_RULES[rule].body == DELIMITER_WORD_BODY) {
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
                    if (!DELIMITER_RULES[closer->rule].rule_of_three || !(closer->can_open || opener->can_close) ||
                        closer->length % 3 == 0 || (opener->length + closer->length) % 3 != 0) {
                        opener_found = true;
                        break;
                    }
                }
                opener = opener->previous;
            }
            old_closer = closer;

            /* Empty script pairs consume both units as text. Neither unit is
             * available to turn a later byte into a different pairing. */
            if (opener_found && DELIMITER_RULES[closer->rule].body == DELIMITER_WORD_BODY &&
                opener->position == closer->position - closer->length) {
                closer = closer->next;
                remove_delimiter(subj, opener);
                remove_delimiter(subj, old_closer);
                continue;
            }

            if (opener_found) {
                reduce_delimiter_range(subj, opener, closer);
                const delimiter_rule_spec *spec = &DELIMITER_RULES[closer->rule];
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
                    remove_delimiter(subj, opener);
                    remove_delimiter(subj, closer);
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
                    remove_delimiter(subj, old_closer);
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
    const bufsize_t minimum_width = DELIMITER_RULES[closer->rule].minimum_width;

    /* A rejected container leaves its authored text intact for every rule. */
    if (!markdown_core_node_can_contain_type(opener_inl->parent, kind)) {
        delimiter *next = closer->next;
        remove_delimiter(subj, opener);
        remove_delimiter(subj, closer);
        return next;
    }

    // Allocate before mutating either run. OOM leaves the source intact and
    // aborts the shared parse transaction.
    inline_node = make_simple(subj->mem, kind);
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
        remove_delimiter(subj, opener);
    } else if (opener_num_chars < minimum_width) {
        remove_delimiter(subj, opener); // A remaining single sign is only text.
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
        // remove empty closer inline
        markdown_core_node_free(closer_inl);
        // remove closer from list
        tmp_delim = closer->next;
        remove_delimiter(subj, closer);
        closer = tmp_delim;
    } else if (closer_num_chars < minimum_width) {
        tmp_delim = closer->next;
        remove_delimiter(subj, closer);
        closer = tmp_delim;
    }

    return closer;
}

// Parse backslash-escape or just a backslash, returning an inline.
static markdown_core_node *handle_backslash(markdown_core_parser *parser, subject *subj) {
    bufsize_t start = subj->pos;
    /* The line frame BEFORE anything is consumed. The hard-break arm below
     * needs it, and reading it after `skip_line_end` would be right only
     * because `skip_line_end` happens not to advance the frame -- an accident,
     * not a contract. `handle_newline` captures its frame first for the same
     * reason, and the two arms must not merely look symmetric. */
    advance(subj);
    unsigned char nextchar = peek_char(subj);
    if (nextchar == ' ') {
        /* A trailing whitespace run cannot belong to a completed script.
         * Leave it to the inherited text/line-ending scanner, including its
         * trimming and hard-break rules. These lookaheads are disjoint: each
         * begins after its own backslash and ends before the next token. */
        bufsize_t end = subj->pos;
        while (end < subj->input.len && !S_is_line_end_char(peek_at(subj, end)) &&
               markdown_core_isspace(peek_at(subj, end))) {
            end++;
            parser->whitespace_work++;
        }
        if ((end == subj->input.len && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(subj->owner->kind)) ||
            (end < subj->input.len && S_is_line_end_char(peek_at(subj, end)))) {
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
    } else if (!is_eof(subj) && skip_line_end(subj)) {
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
        markdown_core_node *hard = make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (hard) {
            /* The break's extent is the backslash and the line ending it
             * escapes; `subj->pos` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            S_place_inline(subj, hard, start, subj->pos - 1);
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

// Clean a URL: remove surrounding whitespace, and remove \ that escape
// punctuation.
//
// A DESTINATION IS REQUIRED (Q26), so there is no absence to report and the
// empty answer is the empty STRING. `[a]()` and `[a](<>)` both wrote a
// destination and wrote nothing in it; returning `MARKDOWN_CORE_CHUNK_EMPTY`
// here handed back NULL data, which every reader downstream had to decide the
// meaning of for itself, and the dump decided `null`.
markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        return markdown_core_chunk_literal("");
    }

    houdini_unescape_html_f(&buf, url->data, url->len);

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

// A TITLE IS OPTIONAL, and this is the one place that knows which of the two
// answers the source gave. A title is delimited -- `"..."`, `'...'`, `(...)`
// -- so the shortest one the source can write is two bytes; a zero-length
// `title` means the scan found no title syntax at all, which is ABSENT, and
// everything else is PRESENT even when the delimiters enclose nothing.
// Nothing downstream re-derives this, which is requirement 14.
markdown_core_optional_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);
    unsigned char first, last;

    if (title->len == 0) {
        return markdown_core_optional_chunk_absent();
    }

    first = title->data[0];
    last = title->data[title->len - 1];

    // remove surrounding quotes if any:
    if ((first == '\'' && last == '\'') || (first == '(' && last == ')') || (first == '"' && last == '"')) {
        houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
    } else {
        houdini_unescape_html_f(&buf, title->data, title->len);
    }

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_optional_chunk_present(markdown_core_chunk_buf_detach(&buf));
}

// Parse an autolink, an HTML comment, or an HTML tag.
// Assumes the subject has a '<' character at the current position.
static bufsize_t scan_inline_html(subject *subj, bufsize_t pos, unsigned *flags, bool *is_comment) {
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

    matchlen = scan_inline_html(subj, subj->pos, &subj->flags, &comment);
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
            return make_comment(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        }
        contents = markdown_core_chunk_dup(&subj->input, subj->pos - 1, matchlen + 1);
        subj->pos += matchlen;
        markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        return node;
    }

    // if nothing matches, just return the opening <:
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("<"));
}

/* Raw labels have one lexical grammar on declarations and occurrences. */
static bufsize_t reference_label_length(const unsigned char *data, bufsize_t length) {
    bufsize_t at = 0;
    while (at < length && at <= MAX_LINK_LABEL_LENGTH && data[at] != '[' && data[at] != ']') {
        if (data[at] == '\\' && at + 1 < length && markdown_core_ispunct(data[at + 1])) {
            at++;
        }
        at++;
    }
    return at;
}

// Parse a link label.  Returns 1 if successful.
// Note:  unescaped brackets are not allowed in labels.
// The label begins with `[` and ends with the first `]` character
// encountered.  Backticks in labels do not start code spans.
static int link_label(subject *subj, markdown_core_chunk *raw_label) {
    bufsize_t startpos = subj->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (peek_char(subj) == '[') {
        advance(subj);
    } else {
        return 0;
    }

    length = reference_label_length(subj->input.data + subj->pos, subj->input.len - subj->pos);
    subj->pos += length;
    if (length > MAX_LINK_LABEL_LENGTH) {
        goto noMatch;
    }
    c = peek_char(subj);

    if (c == ']') { // match found
        *raw_label = markdown_core_chunk_dup(&subj->input, startpos + 1, subj->pos - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        advance(subj); // advance past ]
        return 1;
    }

noMatch:
    subj->pos = startpos; // rewind
    return 0;
}

static bufsize_t manual_scan_link_url_2(markdown_core_chunk *input, bufsize_t offset, markdown_core_chunk *output) {
    bufsize_t i = offset;
    size_t nb_p = 0;

    while (i < input->len) {
        if (input->data[i] == '\\' && i + 1 < input->len && markdown_core_ispunct(input->data[i + 1])) {
            i += 2;
        } else if (input->data[i] == '(') {
            ++nb_p;
            ++i;
            if (nb_p > 32) {
                return -1;
            }
        } else if (input->data[i] == ')') {
            if (nb_p == 0) {
                break;
            }
            --nb_p;
            ++i;
        } else if (markdown_core_isspace(input->data[i])) {
            if (i == offset) {
                return -1;
            }
            break;
        } else {
            ++i;
        }
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset, i - offset, 0};
        *output = result;
    }
    return i - offset;
}

static bufsize_t manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset, markdown_core_chunk *output) {
    bufsize_t i = offset;

    if (i < input->len && input->data[i] == '<') {
        ++i;
        while (i < input->len) {
            if (input->data[i] == '>') {
                ++i;
                break;
            } else if (input->data[i] == '\\') {
                i += 2;
            } else if (input->data[i] == '\n' || input->data[i] == '<') {
                return -1;
            } else {
                ++i;
            }
        }
    } else {
        return manual_scan_link_url_2(input, offset, output);
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset + 1, i - 2 - offset, 0};
        *output = result;
    }
    return i - offset;
}

// Is the label between `[^` and `]` one the document defines?
//
// The span is the same one the reference node's literal is cut from, so the
// question is asked of exactly the bytes that would become the label. The map
// normalizes -- fold, trim, collapse -- on both sides, which is what makes
// `[^Foo Bar]` find `[^foo   bar]`.
static bool S_footnote_label_is_defined(markdown_core_parser *parser, subject *subj, bufsize_t label_start,
                                        bufsize_t after_close) {
    markdown_core_chunk label;
    bool defined;

    if (after_close - label_start < 2) {
        return false;
    }
    /* A borrowed slice of the block's own content: `markdown_core_chunk_dup`
     * aliases, so the only allocation in here is the map's own normalization,
     * and that one reports itself through the map's sticky flag. */
    label = markdown_core_chunk_dup(&subj->input, label_start + 1, after_close - label_start - 2);
    defined = markdown_core_map_lookup(parser->footnote_defs, &label) != NULL;
    return defined;
}

/* Both footnote forms construct the same citation edge. The caller supplies
 * an authored normalized id, or leaves it absent until document finalization. */
/* Cite allocation and item ownership are shared by every referent family. */
static markdown_core_node *new_cite(subject *subj) { return make_simple_subj(subj, MARKDOWN_CORE_NODE_CITE); }

static markdown_core_node *new_citation(subject *subj, markdown_core_node *cite, markdown_core_node *last) {
    markdown_core_node *item = make_simple_subj(subj, MARKDOWN_CORE_NODE_CITATION);
    if (item) {
        item->prev = last;
        if (last) {
            last->next = item;
        } else {
            cite->as.cite->citations = item;
        }
    }
    return item;
}

static markdown_core_node *new_bib_item(subject *subj, markdown_core_node *cite, markdown_core_node *last,
                                        const citation_token *key, bool normal) {
    markdown_core_node *item = new_citation(subj, cite, last);
    if (!item) {
        return NULL;
    }
    item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_BIB;
    item->as.citation->mode = key->suppress ? MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR
                              : normal      ? MARKDOWN_CORE_BIB_MODE_NORMAL
                                            : MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT;
    item->as.citation->value = markdown_core_chunk_dup(&subj->input, key->key_start, key->key_end - key->key_start);
    if (!markdown_core_chunk_to_cstr(subj->mem, &item->as.citation->value)) {
        subj->oom = 1;
        return NULL;
    }
    S_place_inline(subj, item, key->start, key->end - 1);
    return item;
}

static void citation_boundary(subject *subj, citation_token *token, bool field) {
    if (token->boundary) {
        if (field) {
            token->boundary->kind = DELIMITER_AFFIX_BOUNDARY;
        } else {
            remove_delimiter(subj, token->boundary);
        }
        token->boundary = NULL;
    }
}

static void trim_citation_source(subject *subj, bufsize_t *start, bufsize_t *end) {
    while (*start < *end) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(subj->input.data + *start, *end - *start, &scalar);
        subj->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        *start += width;
    }
    while (*end > *start) {
        bufsize_t at = *end - 1;
        while (at > *start && (subj->input.data[at] & 0xc0) == 0x80) {
            at--;
        }
        int32_t scalar;
        markdown_core_utf8proc_iterate(subj->input.data + at, *end - at, &scalar);
        subj->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        /* An escaped ASCII space or line ending is an owned inline token,
         * not raw edge whitespace. Preserve its complete source extent. */
        if ((scalar == ' ' || scalar == '\n') && source_escaped(subj, at, *start)) {
            break;
        }
        *end = at;
    }
}

static int source_compare(int line, int column, int other_line, int other_column) {
    if (line != other_line) {
        return line < other_line ? -1 : 1;
    }
    return (column > other_column) - (column < other_column);
}

/* Only raw whitespace can cross an affix boundary. Such a partial Text has
 * the original input's source-map view; decoded/opaque tokens remain whole. */
static bool trim_affix_node(subject *subj, markdown_core_node *node, bufsize_t start, bufsize_t end) {
    int start_line, start_column, end_line, end_column;
    if (start == end) {
        return false;
    }
    markdown_core_parser_content_place(subj->owner_parser, subj->owner, start, &start_line, &start_column);
    markdown_core_parser_content_end_place(subj->owner_parser, subj->owner, end - 1, &end_line, &end_column);
    if (source_compare(node->end_line, node->end_column, start_line, start_column) < 0 ||
        source_compare(node->start_line, node->start_column, end_line, end_column) > 0) {
        return false;
    }
    bool trim_start = source_compare(node->start_line, node->start_column, start_line, start_column) < 0;
    bool trim_end = source_compare(node->end_line, node->end_column, end_line, end_column) > 0;
    if ((trim_start || trim_end) && node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_chunk *text = node->as.literal;
        bufsize_t from = node->content_mark_offset - subj->owner->content_mark_offset;
        bufsize_t first = trim_start ? start - from : 0;
        bufsize_t length = trim_end && end - from < text->len ? end - from : text->len;
        if (length <= first) {
            return false;
        }
        length -= first;
        if (text->alloc) {
            memmove(text->data, text->data + first, (size_t)length);
        } else {
            text->data += first;
        }
        text->len = length;
        S_place_inline(subj, node, from + first, from + first + length - 1);
    }
    return true;
}

static void take_citation_affix(subject *subj, markdown_core_node **slot, markdown_core_node *first,
                                markdown_core_node *after, bufsize_t start, bufsize_t end) {
    trim_citation_source(subj, &start, &end);
    while (first != after && !subj->oom) {
        markdown_core_node *next = first->next;
        subj->owner_parser->citation_work++;
        if (trim_affix_node(subj, first, start, end)) {
            if (!*slot) {
                *slot = make_simple_subj(subj, MARKDOWN_CORE_NODE_PARAGRAPH);
                if (!*slot) {
                    return;
                }
                S_place_inline(subj, *slot, start, end - 1);
            }
            markdown_core_node_unlink(first);
            append_child(*slot, first);
        } else {
            markdown_core_node_free(first);
        }
        first = next;
    }
}

/* Remove a source delimiter adjacent to an accepted specimen reference.
 * The surviving Text keeps its original map; only its changed edge moves. */
static void remove_specimen_parenthesis(subject *subj, markdown_core_node *text, bool first) {
    assert(text && text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal->len);
    markdown_core_chunk *literal = text->as.literal;
    if (literal->len == 1) {
        markdown_core_node_free(text);
        return;
    }
    if (first) {
        markdown_core_parser_content_place(subj->owner_parser, text, 1, &text->start_line, &text->start_column);
        markdown_core_parser_adopt_content_marks(subj->owner_parser, text, text, 1, literal->len - 1);
        if (literal->alloc) {
            memmove(literal->data, literal->data + 1, (size_t)literal->len - 1);
        } else {
            literal->data++;
        }
    } else {
        markdown_core_parser_content_end_place(subj->owner_parser, text, literal->len - 2, &text->end_line,
                                               &text->end_column);
    }
    literal->len--;
}

static void materialize_citation_key(subject *subj, citation_token *token) {
    if (!token->key || token->node->kind == MARKDOWN_CORE_NODE_CITE || subj->oom) {
        return;
    }
    if (!markdown_core_node_can_contain_type(token->node->parent, MARKDOWN_CORE_NODE_CITE)) {
        return;
    }
    markdown_core_node *cite = new_cite(subj);
    markdown_core_node *item = cite ? new_bib_item(subj, cite, NULL, token, false) : NULL;
    if (!item) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        return;
    }
    bool specimen = !token->suppress && token->key_start == token->start + 1 &&
                    markdown_core_key_index_lookup(&subj->owner_parser->specimen_ids, item->as.citation->value.data,
                                                   item->as.citation->value.len);
    if (specimen) {
        item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
        item->as.citation->mode = 0;
    }
    bufsize_t start = token->start, end = token->end;
    if (specimen && start && peek_at(subj, start - 1) == '(' && peek_at(subj, end) == ')') {
        if (!source_escaped(subj, start - 1, 0) && token->node->prev && token->node->next &&
            token->node->prev->kind == MARKDOWN_CORE_NODE_TEXT && token->node->next->kind == MARKDOWN_CORE_NODE_TEXT) {
            remove_specimen_parenthesis(subj, token->node->prev, false);
            remove_specimen_parenthesis(subj, token->node->next, true);
            start--;
            end++;
        }
    }
    S_place_inline(subj, cite, start, end - 1);
    markdown_core_node_insert_before(token->node, cite);
    markdown_core_node_free(token->node);
    token->node = cite;
}

/* Complete bare keys when another bracket owner wins. Failed groups preserve
 * their unclaimed source tokens; nested committed constructs keep their tree. */
static void finish_citation_tokens(subject *subj, citation_tokens *tokens, bool ordinary) {
    for (citation_token *token = tokens->first; token && !subj->oom; token = token->next) {
        resolve_citation_tail(subj, token, ordinary);
        citation_boundary(subj, token, false);
        if (ordinary) {
            materialize_citation_key(subj, token);
        }
    }
}

static bool citation_group_valid(const citation_tokens *tokens, bool tail) {
    bool key = tail;
    for (citation_token *token = tokens->first; token; token = token->next) {
        if (token->key) {
            key = true;
        } else {
            if (!key) {
                return false;
            }
            key = false;
        }
    }
    return key;
}

static markdown_core_node *make_footnote_cite(subject *subj, bracket *opener, bufsize_t after_close) {
    markdown_core_node *cite = new_cite(subj);
    markdown_core_node *citation = cite ? new_citation(subj, cite, NULL) : NULL;
    if (!citation) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        subj->oom = 1;
        return NULL;
    }
    cite->as.cite->citations = citation;
    citation->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    S_place_inline(subj, cite, opener->position - (opener->kind == BRACKET_FOOTNOTE ? 2 : 1), after_close - 1);
    S_place_inline(subj, citation, opener->position, after_close - 2);
    return cite;
}

/* Claim the parsed body of one balanced bracket pair. Span, links/images,
 * and document-owned inline notes share this transfer; their callers decide
 * where the resulting owner lives and when its delimiter boundary closes. */
static void take_bracket_content(markdown_core_parser *parser, bracket *opener, markdown_core_node *owner) {
    markdown_core_node *child = opener->inl_text->next;
    while (child != opener->close_text) {
        markdown_core_node *next = child->next;
        markdown_core_node_unlink(child);
        append_child(owner, child);
        parser->bracket_work++;
        child = next;
    }
}

/* Non-media bracket alternatives own '[' through the suffix. An authored
 * image bang remains an ordinary Text sibling under bracket rules B3-B6. */
static void replace_bracket_opener(subject *subj, bracket *opener, markdown_core_node *replacement) {
    if (opener->kind == BRACKET_IMAGE) {
        opener->inl_text->as.literal->len = 1;
        S_place_inline(subj, opener->inl_text, opener->position - 2, opener->position - 2);
        markdown_core_node_insert_after(opener->inl_text, replacement);
    } else {
        markdown_core_node_insert_before(opener->inl_text, replacement);
        markdown_core_node_free(opener->inl_text);
    }
}

static markdown_core_node *close_inline_footnote(markdown_core_parser *parser, subject *subj, bracket *opener) {
    markdown_core_node *cite, *footnote;
    /* The consumed body is inspected once by parse_inline, never once per
     * ancestor. Invalid openers keep their already parsed content as text. */
    if (subj->nonblank_end <= opener->position ||
        !markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
        subj->no_link_openers = opener->outer_no_link_openers;
        pop_bracket(subj);
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }
    cite = make_footnote_cite(subj, opener, subj->pos);
    footnote = cite ? make_simple(subj->mem, MARKDOWN_CORE_NODE_FOOTNOTE) : NULL;
    if (!footnote) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        subj->oom = 1;
        pop_bracket(subj);
        return NULL;
    }
    S_place_inline(subj, footnote, opener->position - 2, subj->pos - 1);
    finish_citation_tokens(subj, &opener->citations, true);
    process_delimiters(parser, subj, opener->position, opener->delim_end);
    take_bracket_content(parser, opener, footnote);
    markdown_core_node_insert_before(opener->inl_text, cite);
    if (!markdown_core_parser_register_definition(parser, footnote, cite->as.cite->citations)) {
        markdown_core_node_free(footnote);
        subj->oom = 1;
    }
    markdown_core_node_free(opener->inl_text);
    subj->no_link_openers = opener->outer_no_link_openers;
    pop_bracket(subj);
    return NULL;
}

/* Positive 32-bit components bound inspection even for arbitrarily long digit
 * runs. No decoded character or partially valid suffix becomes a dimension. */
static bool dimension_component(const unsigned char *s, bufsize_t *pos, bufsize_t end, int32_t *value, size_t *work) {
    if (*pos == end || s[*pos] < '1' || s[*pos] > '9') {
        return false;
    }
    int32_t number = 0;
    while (*pos < end && s[*pos] >= '0' && s[*pos] <= '9') {
        (*work)++;
        int digit = s[(*pos)++] - '0';
        if (number > (INT32_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

bool markdown_core_parse_dimensions(markdown_core_chunk label, bufsize_t suffix, bufsize_t separator_length,
                                    markdown_core_dimensions *value, size_t *work) {
    bufsize_t pos = suffix + separator_length;
    markdown_core_dimensions parsed = {0};
    (*work)++;
    if ((suffix > 0 && markdown_core_isspace(label.data[suffix - 1])) ||
        !dimension_component(label.data, &pos, label.len, &parsed.width, work)) {
        return false;
    }
    if (pos < label.len && label.data[pos] == 'x') {
        int32_t height;
        pos++;
        if (!dimension_component(label.data, &pos, label.len, &height, work)) {
            return false;
        }
        parsed.height = (markdown_core_optional_i64){true, height};
    }
    if (pos != label.len) {
        return false;
    }
    *value = parsed;
    return true;
}

static void apply_image_dimensions(subject *subj, const bracket *opener, markdown_core_node *image, bufsize_t end) {
    /* Earlier inline allocation failure may have omitted the final text run.
     * The transaction is already failed; do not consume its incomplete tree. */
    if (subj->oom || subj->owner_parser->oom) {
        return;
    }
    bufsize_t suffix = opener->image_pipe >= 0 ? opener->image_pipe : opener->position;
    markdown_core_dimensions dimensions;
    markdown_core_chunk label = markdown_core_chunk_dup(&subj->input, opener->position, end - opener->position);
    if (!markdown_core_parse_dimensions(label, suffix - opener->position, opener->image_pipe >= 0 ? 1 : 0, &dimensions,
                                        &subj->owner_parser->dimension_work)) {
        return;
    }

    /* A successful suffix contains only ordinary ASCII text and belongs to
     * the final text run at this bracket depth. Remove it before delimiter
     * reduction; the prefix keeps its nodes and its original source map. */
    markdown_core_node *tail = image->last_child;
    assert(tail && tail->kind == MARKDOWN_CORE_NODE_TEXT && tail->as.literal->len >= end - suffix);
    bufsize_t start = end - tail->as.literal->len;
    tail->as.literal->len -= end - suffix;
    if (tail->as.literal->len == 0) {
        markdown_core_node_free(tail);
    } else {
        S_place_inline(subj, tail, start, suffix - 1);
    }
    image->as.link->dimensions.value = dimensions;
    image->as.link->dimensions.has_value = true;
}

static bool close_bibliography(markdown_core_parser *parser, subject *subj, bracket *opener) {
    bool tail = opener->author && peek_char(subj) != '(' && peek_char(subj) != '[';
    if (peek_at(subj, opener->position) == '^' || !citation_group_valid(&opener->citations, tail) ||
        !markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
        return false;
    }
    bool first = !tail;
    for (citation_token *token = opener->citations.first; token && !subj->oom; token = token->next) {
        if (!token->key) {
            citation_boundary(subj, token, true);
            first = true;
        } else if (first) {
            resolve_citation_tail(subj, token, false);
            citation_boundary(subj, token, true);
            first = false;
        } else {
            resolve_citation_tail(subj, token, true);
            citation_boundary(subj, token, false);
            materialize_citation_key(subj, token);
        }
    }
    if (subj->oom) {
        pop_bracket(subj);
        return true;
    }
    process_delimiters(parser, subj, opener->position, opener->delim_end);
    markdown_core_node *cite = new_cite(subj);
    if (!cite) {
        pop_bracket(subj);
        return true;
    }
    S_place_inline(subj, cite, tail ? opener->author->start : opener->position - 1, subj->pos - 1);
    markdown_core_node *content = opener->inl_text->next;
    if (tail) {
        markdown_core_node *old = opener->author->node;
        markdown_core_node_insert_before(old, cite);
        while (old != content) {
            markdown_core_node *next = old->next;
            markdown_core_node_free(old);
            old = next;
        }
        opener->author->node = cite;
    } else {
        replace_bracket_opener(subj, opener, cite);
    }
    bufsize_t item_start = opener->position;
    citation_token *token = opener->citations.first;
    markdown_core_node *last = NULL;
    bool author_item = tail;
    while ((author_item || token) && !subj->oom) {
        citation_token *key = author_item ? opener->author : token;
        assert(key->key);
        citation_token *separator = author_item ? token : key->next;
        while (separator && separator->key) {
            separator = separator->next;
        }
        bufsize_t item_end = separator ? separator->start : subj->pos - 1;
        bufsize_t scope_start = author_item ? key->start : item_start, scope_end = item_end;
        trim_citation_source(subj, &scope_start, &scope_end);
        markdown_core_node *item = new_bib_item(subj, cite, last, key, !author_item);
        if (!item) {
            break;
        }
        last = item;
        S_place_inline(subj, item, scope_start, scope_end - 1);
        if (!author_item) {
            take_citation_affix(subj, &item->as.citation->prefix, content, key->node, item_start, key->start);
            content = key->node->next;
            markdown_core_node_free(key->node);
        }
        take_citation_affix(subj, &item->as.citation->suffix, content, separator ? separator->node : opener->close_text,
                            author_item ? item_start : key->end, item_end);
        author_item = false;
        if (separator) {
            content = separator->node->next;
            markdown_core_node_free(separator->node);
            item_start = separator->end;
            token = separator->next;
        } else {
            token = NULL;
        }
    }
    if (tail) {
        opener->author->end = subj->pos;
    }
    pop_bracket(subj);
    return true;
}

// Return a link, an image, or a literal close bracket.
static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, subject *subj) {
    bufsize_t initial_pos, after_link_text_pos;
    bufsize_t endurl, starttitle, endtitle, endall;
    bufsize_t sps, n;
    /* The definition a reference resolved to, or NULL on the direct path. */
    markdown_core_map_record *record = NULL;
    markdown_core_chunk url_chunk, title_chunk;
    /* SET HERE AND NOT ONLY ON THE INLINE-LINK PATH. A reference reaches `match`
     * with `record` set and never reads these, but MSVC cannot follow that
     * across the label and rejects the function under /WX with C4701, which
     * is a Windows-only diagnostic no other host reports. Giving them the
     * absent value costs nothing and says what the unset state means. */
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_optional_chunk title = {MARKDOWN_CORE_CHUNK_EMPTY, false};
    bracket *opener;
    markdown_core_node *inl;
    markdown_core_chunk raw_label;
    int found_label;
    bool is_image;
    bool link_allowed;
    bool explicit_tail = false;

    parser->bracket_work++;
    advance(subj); // advance past ]
    initial_pos = subj->pos;

    // get last [ or ![
    opener = subj->last_bracket;

    if (opener == NULL) {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }

    if (opener->kind == BRACKET_FOOTNOTE) {
        return close_inline_footnote(parser, subj, opener);
    }

    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    is_image = opener->kind == BRACKET_IMAGE;

    link_allowed = is_image || !subj->no_link_openers;

    after_link_text_pos = subj->pos;

    // First, look for an inline link.
    if (link_allowed && peek_char(subj) == '(' && ((sps = scan_spacechars(&subj->input, subj->pos + 1)) > -1) &&
        ((n = manual_scan_link_url(&subj->input, subj->pos + 1 + sps, &url_chunk)) > -1)) {

        // try to parse an explicit link:
        endurl = subj->pos + 1 + sps + n;
        starttitle = endurl + scan_spacechars(&subj->input, endurl);

        // ensure there are spaces btw url and title
        endtitle = (starttitle == endurl) ? starttitle : starttitle + scan_link_title(&subj->input, starttitle);

        endall = endtitle + scan_spacechars(&subj->input, endtitle);

        if (peek_at(subj, endall) == ')') {
            explicit_tail = true;
            subj->pos = endall + 1;

            title_chunk = markdown_core_chunk_dup(&subj->input, starttitle, endtitle - starttitle);
            {
                int lost = 0;
                url = markdown_core_clean_url(subj->mem, &url_chunk, &lost);
                title = markdown_core_clean_title(subj->mem, &title_chunk, &lost);
                if (lost) {
                    subj->oom = 1;
                }
            }
            markdown_core_chunk_free(subj->mem, &url_chunk);
            markdown_core_chunk_free(subj->mem, &title_chunk);
            goto match;

        } else {
            // it could still be a shortcut reference link
            subj->pos = after_link_text_pos;
        }
    }

    // Next, look for a following [link label] that matches in refmap.
    // skip spaces
    raw_label = markdown_core_chunk_literal("");
    found_label = link_label(subj, &raw_label);
    explicit_tail = found_label;
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        subj->pos = initial_pos;
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(subj->mem, &raw_label);
        raw_label = markdown_core_chunk_dup(&subj->input, opener->position, initial_pos - opener->position - 1);
        found_label = true;
    }

    /* `[t][l]`, `[l][]` and `[l]` resolve identically and to the same node: the
     * `Link` or `Media` the definition names (M2). Nothing records which of the
     * three spellings the author wrote, and nothing downstream can recover it
     * -- the module states one node for every successful form. */
    if (link_allowed && found_label) {
        record = markdown_core_map_lookup(subj->refmap, &raw_label);
    }
    markdown_core_chunk_free(subj->mem, &raw_label);
    if (record && explicit_tail) {
        goto match;
    }

    /* A Span is independent of link eligibility: a complete link may be
     * inside it. Only direct/full/collapsed link tails claim these bytes
     * first; a shortcut is considered after the attribute alternative. */
    subj->pos = initial_pos;
    {
        markdown_core_attributes attributes = {0};
        bufsize_t end;
        if (markdown_core_inline_parser_attributes(subj, initial_pos, &attributes, &end)) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_SPAN)) {
                markdown_core_attributes_free(subj->mem, &attributes);
                goto no_match;
            }
            inl = make_simple_subj(subj, MARKDOWN_CORE_NODE_SPAN);
            if (!inl) {
                markdown_core_attributes_free(subj->mem, &attributes);
                pop_bracket(subj);
                return NULL;
            }
            inl->attributes = attributes;
            subj->pos = end;
            S_place_inline(subj, inl, opener->position - 1, end - 1);
            finish_citation_tokens(subj, &opener->citations, true);
            process_delimiters(parser, subj, opener->position, opener->delim_end);
            take_bracket_content(parser, opener, inl);
            replace_bracket_opener(subj, opener, inl);
            pop_bracket(subj);
            return NULL;
        }
    }
    if (opener->author && !opener->close_text && peek_char(subj) != '(' && peek_char(subj) != '[' &&
        citation_group_valid(&opener->citations, true)) {
        markdown_core_node *close = make_str(subj, initial_pos - 1, initial_pos - 1, markdown_core_chunk_literal("]"));
        delimiter *end = close ? push_delimiter_entry(subj, DELIMITER_CITATION_TOKEN, initial_pos) : NULL;
        if (!end) {
            if (close) {
                markdown_core_node_free(close);
            }
            subj->oom = 1;
            return NULL;
        }
        opener->close_text = close;
        opener->close_position = initial_pos - 1;
        opener->delim_end = end;
        opener->pending_no_link_openers = subj->no_link_openers;
        opener->pending_reference = record;
        opener->author->tail = opener;
        opener->pending_next = subj->pending_brackets;
        if (subj->pending_brackets) {
            subj->pending_brackets->pending_previous = opener;
        }
        subj->pending_brackets = opener;
        subj->last_bracket = opener->previous;
        return close;
    }
    if (close_bibliography(parser, subj, opener)) {
        return NULL;
    }
    if (record) {
        goto match;
    }

    // If we fall through to here, it means we didn't match a link.
    // What if we're a footnote link?
    if (opener->inl_text->next && opener->inl_text->next->kind == MARKDOWN_CORE_NODE_TEXT) {

        markdown_core_chunk *literal = opener->inl_text->next->as.literal;

        // A footnote call opens with a caret the SOURCE spells literally.
        //
        // This used to test the decoded first byte, so `[\^a]` and `[&#94;a]`
        // opened calls too — and neither could ever resolve, because the label
        // was reconstructed from a different coordinate space than the one the
        // lookup key came from. What they produced instead was a rebuilt `[^`
        // prefix over decoded bytes: `[\^abc] x` came back as `[^^abc] x`, an
        // invented caret, and `[&#94;a]` as `[^#94;a]`.
        //
        // `opener->position` is the byte after the '[', which is where the
        // caret must be. The bounds test comes first because that is the order
        // a subscript and its guard belong in -- D4 is what happens when they
        // are the other way round. It is REDUNDANT here and the proof is worth
        // writing down rather than rediscovering: reaching this function means
        // a ']' was consumed, and that ']' is after the '[', so
        // `opener->position <= initial_pos - 2 < subj->input.len`. No mutant
        // kills it, measured; it is kept because a reader should not have to
        // reconstruct that argument before touching the line.
        //
        // AND THE DOCUMENT DEFINES THAT LABEL. Both halves of Step 9a's rule
        // are here now, and the second one is what makes the failure path
        // ordinary: a `[^label]` nothing defines is not a footnote call, so it
        // is an unmatched `[`, which CommonMark specifies -- remove the
        // delimiter-stack entry, emit a literal `]`, and leave the interior
        // alone. Every one of its three failure branches says NOT re-parenting
        // is what failure means, and the interior nodes exist because core
        // inline parsing built them before any footnote code ran (§5.7, Q2).
        // What used to happen instead was that the call succeeded on the caret
        // alone, and the post-pass that could not resolve it replaced the whole
        // span with one flat literal -- freeing nodes core had already built,
        // for a construct that turned out not to exist.
        //
        // The definition set is filled by the block phase, which has finished
        // by the time any inline is parsed, so "defines" is answered over the
        // WHOLE document here and not over a prefix of it.
        /* THE CARET IS THE EVIDENCE, and it is separated from the definedness
         * test so that the case where the first holds and the second does not
         * can be reported. `[^x]` is footnote syntax and nothing else; a reader
         * who writes it and gets prose has no other way to find out. */
        bool caret_written = opener->position < subj->input.len && subj->input.data[opener->position] == '^' &&
                             (literal->len > 1 || opener->inl_text->next->next);
        if (caret_written && S_footnote_label_is_defined(parser, subj, opener->position, initial_pos)) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
                goto no_match;
            }

            // Before we got this far, the `handle_close_bracket` function may have
            // advanced the current state beyond our footnote's actual closing
            // bracket, ie if it went looking for a `link_label`.
            // Let's just rewind the subject's position:
            subj->pos = initial_pos;

            markdown_core_node *fnref = make_footnote_cite(subj, opener, initial_pos);
            if (!fnref) {
                pop_bracket(subj);
                return NULL;
            }
            markdown_core_chunk label =
                markdown_core_chunk_dup(&subj->input, opener->position + 1, initial_pos - opener->position - 2);
            int lost = 0;
            unsigned char *id = normalize_map_label(subj->mem, &label, &lost);
            if (!id) {
                subj->oom = 1;
                markdown_core_node_free(fnref);
                pop_bracket(subj);
                return NULL;
            }
            markdown_core_chunk *value = &fnref->as.cite->citations->as.citation->value;
            value->data = id;
            value->len = (bufsize_t)strlen((const char *)id);
            value->alloc = 1;

            process_delimiters(parser, subj, opener->position, opener->delim_end);
            // sometimes, the footnote reference text gets parsed into multiple nodes
            // i.e. '[^example]' parsed into '[', '^exam', 'ple]'.
            // this happens for ex with the autolink extension. when the autolinker
            // finds the 'w' character, it will split the text into multiple nodes
            // in hopes of being able to match a 'www.' substring.
            //
            // because this function is called one character at a time via the
            // `parse_inlines` function, and the current subj->pos is pointing at the
            // closing ] brace, and because we copy all the text between the [ ]
            // braces, we should be able to safely ignore and delete any nodes after
            // the opener->inl_text->next.
            //
            // therefore, here we walk thru the list and free them all up
            /* A valid definition label contains no ']'; a completed inline
             * footnote necessarily does. This label therefore cannot own a
             * committed Footnote from the parser collection. */
            markdown_core_node *next_node;
            markdown_core_node *current_node = opener->inl_text->next;
            while (current_node) {
                next_node = current_node->next;
                markdown_core_node_free(current_node);
                current_node = next_node;
            }

            replace_bracket_opener(subj, opener, fnref);
            pop_bracket(subj);
            return NULL;
        }
    }

no_match:
    finish_citation_tokens(subj, &opener->citations, false);
    pop_bracket(subj); // remove this opener from delimiter list
    subj->pos = initial_pos;
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));

match:
    finish_citation_tokens(subj, &opener->citations, true);
    if (!markdown_core_node_can_contain_type(opener->inl_text->parent,
                                             is_image ? MARKDOWN_CORE_NODE_MEDIA : MARKDOWN_CORE_NODE_LINK)) {
        markdown_core_chunk_free(subj->mem, &url);
        markdown_core_optional_chunk_free(subj->mem, &title);
        goto no_match;
    }
    inl = make_simple(subj->mem, is_image ? MARKDOWN_CORE_NODE_MEDIA : MARKDOWN_CORE_NODE_LINK);
    if (inl && record) {
        /* A RESOLVED REFERENCE IS THE LINK OR MEDIA IT NAMES (M2), and it reads
         * its destination and title through the definition's resource, which
         * the map owns once and every occurrence shares. Nothing is copied, so
         * there is nothing to charge and no budget can make whether a reference
         * resolves depend on how many resolved before it (D9). The occurrence
         * keeps its own scope, below: the definition's range is never copied,
         * unioned or substituted into it. */
        assert(record->resource != NULL);
        markdown_core_resource_retain(record->resource);
        inl->as.link->resource = record->resource;
    } else if (inl) {
        inl->as.link->resource = markdown_core_resource_new(subj->mem, url, title);
        if (!inl->as.link->resource) {
            markdown_core_node_free(inl);
            inl = NULL;
        }
    }
    if (!inl) {
        subj->oom = 1;
        if (!record) {
            markdown_core_chunk_free(subj->mem, &url);
            markdown_core_optional_chunk_free(subj->mem, &title);
        }
        pop_bracket(subj);
        subj->pos = initial_pos;
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }
    /* REQUIREMENT 11b: the brackets, and whatever follows the closing one --
     * `(...)` with the destination and title, or `[label]` -- are the link's
     * markers. They were claimed CONTENT as they were read, because an
     * unmatched `[` is its own literal; these claims are later and win. The
     * children keep the claims they made for themselves. */
    // A link starts at its own '[' and ends at its closing ')' or ']', and the
    // two need not be on the same line. Taking BOTH from subj->line made a link
    // start where it ENDED: `[a\nb](/u)` reported Link 2:1..2:6 around a child
    // Text at 1:2 -- a node that begins after its own first child.
    inl->start_line = opener->inl_text->start_line;
    inl->start_column = opener->inl_text->start_column;
    if (explicit_tail) {
        attach_inline_attributes(subj, inl, opener->position - 1);
        inl->start_column = opener->inl_text->start_column;
    }
    S_place_inline(subj, inl, opener->position - 1, subj->pos - 1);
    inl->start_line = opener->inl_text->start_line;
    inl->start_column = opener->inl_text->start_column;
    // And the destination and title are scanned by manual_scan_link_url and
    // scan_link_title, which move subj->pos without ever passing through
    // handle_newline -- so a line ending inside `(...)` is invisible to the
    // subject, and every later node in the paragraph inherits the error.
    // The extent is projected from the two offsets, which is the repair inline
    // code and raw HTML already
    // use; it walks only [initial_pos, subj->pos), which is what the bracket
    // handler consumed for itself. Counting from the OPENING bracket instead
    // would count the label's own newlines a second time -- measured,
    // `[a\nb](/u) tail` then reports line 3 of a two-line document.
    markdown_core_node_insert_before(opener->inl_text, inl);
    take_bracket_content(parser, opener, inl);

    if (is_image) {
        apply_image_dimensions(subj, opener, inl, initial_pos - 1);
    }

    // Free the bracket [:
    markdown_core_node_free(opener->inl_text);

    process_delimiters(parser, subj, opener->position, opener->delim_end);
    pop_bracket(subj);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        subj->no_link_openers = true;
    }

    return NULL;
}

static void resume_citation_tail(subject *subj, citation_token *token, bool ordinary) {
    bracket *pending = token->tail;
    if (!pending || subj->oom || subj->owner_parser->oom) {
        return;
    }
    token->tail = NULL;
    bracket *saved_bracket = subj->last_bracket;
    bufsize_t saved_pos = subj->pos;
    bool saved_no_links = subj->no_link_openers;
    markdown_core_node *close = pending->close_text;
    pending->previous = saved_bracket;
    pending->author = ordinary ? token : NULL;
    subj->last_bracket = pending;
    subj->pos = pending->close_position;
    subj->no_link_openers = pending->pending_no_link_openers;
    markdown_core_node *literal = handle_close_bracket(subj->owner_parser, subj);
    if (literal) {
        markdown_core_node_free(literal);
    } else if (!subj->oom && !subj->owner_parser->oom) {
        markdown_core_node_free(close);
    }
    subj->pos = saved_pos;
    subj->last_bracket = saved_bracket;
    subj->no_link_openers |= saved_no_links;
}

typedef struct {
    citation_token *key, *next;
    bool ordinary, partitioned, first;
} citation_resolution;

static citation_resolution citation_resolution_for(citation_token *key, bool ordinary) {
    bracket *pending = key->tail;
    bool group = !ordinary && citation_group_valid(&pending->citations, false);
    return (citation_resolution){key, pending->citations.first, ordinary, ordinary || group, !ordinary};
}

/* Resolve bracket dependencies in postorder without using the C call stack.
 * Every token list and suspended range is consumed once, including adversarial
 * chains of author keys whose possible tails contain more author keys. */
static void resolve_citation_tail(subject *subj, citation_token *token, bool ordinary) {
    if (!token->tail || subj->oom || subj->owner_parser->oom) {
        return;
    }
    citation_resolution *stack = NULL;
    size_t count = 0, capacity = 0;
    citation_resolution next = citation_resolution_for(token, ordinary);
    for (;;) {
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 8;
            if (grown > SIZE_MAX / sizeof(*stack)) {
                subj->oom = 1;
                break;
            }
            void *values = subj->mem->realloc(stack, grown * sizeof(*stack));
            if (!values) {
                subj->oom = 1;
                break;
            }
            stack = values;
            capacity = grown;
        }
        stack[count++] = next;
        bool descend = false;
        while (count && !subj->oom && !subj->owner_parser->oom) {
            citation_resolution *frame = &stack[count - 1];
            citation_token *child = frame->next;
            if (!child) {
                resume_citation_tail(subj, frame->key, frame->ordinary);
                count--;
                continue;
            }
            frame->next = child->next;
            subj->owner_parser->citation_work++;
            bool child_ordinary;
            if (frame->partitioned) {
                child_ordinary = !frame->first;
                frame->first = !child->key;
            } else {
                child_ordinary = frame->key->tail->pending_reference != NULL;
            }
            if (child->tail) {
                next = citation_resolution_for(child, child_ordinary);
                descend = true;
                break;
            }
        }
        if (!descend || subj->oom || subj->owner_parser->oom) {
            break;
        }
    }
    subj->mem->free(stack);
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
    if (peek_at(subj, subj->pos) == '\r') {
        advance(subj);
    }
    if (peek_at(subj, subj->pos) == '\n') {
        advance(subj);
    }
    // skip spaces at beginning of line
    skip_spaces(subj);
    if (nlpos > 1 && peek_at(subj, nlpos - 1) == ' ' && peek_at(subj, nlpos - 2) == ' ') {
        brk = make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        brk = make_simple_subj(subj, MARKDOWN_CORE_NODE_SOFT_BREAK);
    }
    if (brk) {
        // The two spaces of a hard break stay with the text they follow, as
        // upstream also has them, so both forms own exactly the line ending.
        S_place_inline(subj, brk, nlpos, nlpos);
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
        } else if ((c == '-' && peek_at(subj, n + 1) != '@') || (c == ';' && !subj->last_bracket) ||
                   (c == '@' && (!citation_opener(subj, n) || !citation_key_follows(subj, n + 1)))) {
            /* These bytes have no citation-token role in this context. */
            n++;
        } else if (core_delimiter_rule(c) != MARKDOWN_CORE_DELIM_RULE_NONE) {
            const core_delimiter_run *run = scan_core_delimiter(subj, n);
            if (core_delimiter_needs_stack(subj, run)) {
                assert(n > subj->pos);
                return n;
            }
            // A run that cannot delimit belongs to the current text slice.
            n = run->end;
        } else if (n > subj->pos && (c != '^' || (n + 1 < subj->input.len && subj->input.data[n + 1] == '['))) {
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
    case '=':
    case '+':
    case '[':
    case ']':
    case '<':
    case '!':
    case '^':
    case '@':
    case '-':
    case ';':
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
static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent) {
    markdown_core_node *new_inl = NULL;
    markdown_core_chunk contents;
    unsigned char c;
    bufsize_t startpos, endpos;
    bufsize_t token_start = subj->pos;
    c = peek_char(subj);
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
    if (subj->pos == subj->heading_content_end) {
        bufsize_t end;
        if (markdown_core_attributes_parse(&subj->attributes, subj->heading_attributes_start, &parent->attributes,
                                           &end)) {
            subj->heading_label_end = subj->heading_content_end;
            subj->pos = subj->input.len;
        }
        if (subj->attributes.oom) {
            subj->oom = 1;
        }
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
    case '@':
    case '-':
        new_inl = read_citation_token(subj, true);
        if (!new_inl && !subj->oom) {
            goto text;
        }
        break;
    case ';':
        if (!subj->last_bracket) {
            goto text;
        }
        new_inl = read_citation_token(subj, false);
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
    case '_':
    case '=':
    case '+': {
        const core_delimiter_run *run = scan_core_delimiter(subj, subj->pos);
        if (!core_delimiter_needs_stack(subj, run)) {
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
    case '^':
        if (peek_char_n(subj, 1) != '[') {
            new_inl = handle_delim(subj, scan_core_delimiter(subj, subj->pos));
            break;
        }
        subj->pos += 2;
        new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_literal("^["));
        if (new_inl) {
            push_bracket(subj, BRACKET_FOOTNOTE, new_inl);
        }
        break;
    case '~':
        if (peek_char_n(subj, 1) == '~') {
            new_inl = try_extensions(parser, parent, c, subj);
        } else {
            core_delimiter_run run = {.start = subj->pos,
                                      .end = subj->pos + 1,
                                      .rule = MARKDOWN_CORE_DELIM_RULE_SUBSCRIPT,
                                      .can_open = true,
                                      .can_close = true};
            new_inl = handle_delim(subj, &run);
        }
        break;
    case '[':
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl != NULL || parser->oom || subj->oom) {
            break;
        }
        advance(subj);
        /* CONTENT until it matches: an unmatched `[` IS its own literal, and
         * `handle_close_bracket` re-claims it MARKER for the link it opens. */
        new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("["));
        if (new_inl) {
            push_bracket(subj, BRACKET_LINK, new_inl);
        }
        break;
    case ']':
        /* Opaque tokens consume their brackets before this dispatch. Every
         * remaining close bracket belongs to the shared bracket procedure;
         * no extension dispatches on ']'. Never rescan the delimiter stack. */
        new_inl = handle_close_bracket(parser, subj);
        break;
    case '!':
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl != NULL || parser->oom || subj->oom) {
            break;
        }

        advance(subj);
        if (peek_char(subj) == '[' && peek_char_n(subj, 1) != '^') {
            advance(subj);
            new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_literal("!["));
            if (new_inl) {
                push_bracket(subj, BRACKET_IMAGE, new_inl);
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
        if (subj->last_bracket && subj->last_bracket->kind == BRACKET_IMAGE) {
            for (bufsize_t i = subj->pos; i < endpos; i++) {
                parser->dimension_work++;
                if (subj->input.data[i] == '|') {
                    subj->last_bracket->image_pipe = i;
                }
            }
        }
        contents = markdown_core_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
        startpos = subj->pos;
        subj->pos = endpos;

        // if we're at a newline, strip trailing spaces.
        if (S_is_line_end_char(peek_char(subj))) {
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
        append_child(parent, new_inl);
        bool has_fields = false;
        markdown_core_visit_inline_subtrees(new_inl, has_inline_field, &has_fields);
        if (has_fields) {
            delimiter *entry = push_delimiter_entry(subj, DELIMITER_FIELD, subj->pos);
            if (entry) {
                entry->node = new_inl;
            }
        }
    }
    return 1;
}

static void start_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap,
                          subject *subj) {
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
    subject_from_buf(parser, parser->mem, parent->start_line, subj, &content, refmap);
    subj->owner = parent;
    /* Block buffers include their terminating line ending. An inline field
     * ends at its owner's delimiter: its trailing spaces are body content. */
    if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P(parent->kind)) {
        markdown_core_chunk_rtrim(&subj->input);
    }

    if (parent->kind == MARKDOWN_CORE_NODE_HEADING) {
        bufsize_t line = subj->input.len;
        while (line > 0 && !S_is_line_end_char(subj->input.data[line - 1])) {
            line--;
        }
        subj->attributes =
            (markdown_core_attribute_parser){.mem = parser->mem, .data = subj->input.data, .length = subj->input.len};
        subj->heading_attributes_start = markdown_core_attributes_tail(&subj->attributes, line, subj->input.len);
        if (subj->heading_attributes_start >= 0) {
            bufsize_t end = subj->heading_attributes_start;
            while (end > line && (subj->input.data[end - 1] == ' ' || subj->input.data[end - 1] == '\t')) {
                end--;
            }
            if (!parent->as.heading->setext) {
                bufsize_t hashes = end;
                while (hashes > line && subj->input.data[hashes - 1] == '#') {
                    hashes--;
                }
                if (hashes < end &&
                    (hashes == line || (subj->input.data[hashes - 1] == ' ' || subj->input.data[hashes - 1] == '\t'))) {
                    end = hashes;
                    while (end > line && (subj->input.data[end - 1] == ' ' || subj->input.data[end - 1] == '\t')) {
                        end--;
                    }
                }
            }
            subj->heading_content_end = end;
        } else if (!parent->as.heading->setext) {
            bufsize_t hashes = subj->input.len;
            while (hashes > 0 && subj->input.data[hashes - 1] == '#') {
                hashes--;
            }
            if (hashes < subj->input.len &&
                (hashes == 0 || (subj->input.data[hashes - 1] == ' ' || subj->input.data[hashes - 1] == '\t'))) {
                subj->input.len = hashes;
                markdown_core_chunk_rtrim(&subj->input);
            }
        }
        if (subj->attributes.oom) {
            subj->oom = 1;
        }
    }

    subj->heading_label_end = subj->input.len;
}

static void clear_inlines(subject *subj) {
    markdown_core_parser *parser = subj->owner_parser;
    free_citation_tokens(subj, &subj->citations);
    subj->mem->free(subj->citation_braces.entries);
    subj->citation_braces = (citation_brace_index){0};
    subj->mem->free(subj->backticks);
    subj->backticks = NULL;
    // free bracket and delim stack
    while (subj->last_delim) {
        remove_delimiter(subj, subj->last_delim);
    }
    while (subj->last_bracket) {
        pop_bracket(subj);
    }
    while (subj->pending_brackets) {
        bracket *next = subj->pending_brackets->pending_next;
        free_citation_tokens(subj, &subj->pending_brackets->citations);
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

static bool finish_inlines(markdown_core_parser *parser, subject *subj) {
    while (!parser->oom && !subj->oom) {
        complete_inline_token(parser, subj);
        if (parser->oom || subj->oom || is_eof(subj) || !parse_inline(parser, subj, subj->owner)) {
            break;
        }
    }
    if (!parser->oom && !subj->oom) {
        for (bracket *open = subj->last_bracket; open; open = open->previous) {
            finish_citation_tokens(subj, &open->citations, true);
        }
        finish_citation_tokens(subj, &subj->citations, true);
        process_delimiters(parser, subj, 0, NULL);
    }
    bool whitespace = subj->last_delim && subj->last_delim->kind == DELIMITER_BOUNDARY;
    clear_inlines(subj);
    return whitespace;
}

bool markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_map *refmap) {
    subject subj;
    start_inlines(parser, parent, refmap, &subj);
    return finish_inlines(parser, &subj);
}

void markdown_core_prepare_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    subject subj;
    start_inlines(parser, heading->node, parser->refmap, &subj);
    while (!parser->oom && !subj.oom) {
        unsigned char c = peek_char(&subj);
        /* Attribute ownership and opaque tokens are decided by the same
         * cursor as every inline. A live bracket makes this declaration
         * unwritable as a reference label; only its remaining inlines depend
         * on the document's completed symbol table. */
        if ((subj.last_delim && subj.last_delim->kind == DELIMITER_FIELD) ||
            (subj.pos != subj.heading_content_end && subj.pos >= subj.opaque_end &&
             (c == '[' || c == ']' || ((c == '!' || c == '^') && peek_char_n(&subj, 1) == '[')))) {
            heading->pending = parser->mem->calloc(1, sizeof(subj));
            if (heading->pending) {
                *heading->pending = subj;
                return;
            }
            subj.oom = 1;
            break;
        }
        if (is_eof(&subj) || !parse_inline(parser, &subj, heading->node)) {
            break;
        }
    }
    if (!parser->oom && !subj.oom) {
        finish_citation_tokens(&subj, &subj.citations, true);
        process_delimiters(parser, &subj, 0, NULL);
        markdown_core_chunk label = {subj.input.data, subj.heading_label_end, 0};
        if (label.len > 0 && label.len <= MAX_LINK_LABEL_LENGTH &&
            reference_label_length(label.data, label.len) == label.len) {
            markdown_core_resource *resource = markdown_core_resource_new(parser->mem, markdown_core_chunk_literal(""),
                                                                          markdown_core_optional_chunk_absent());
            if (!resource) {
                subj.oom = 1;
            } else {
                markdown_core_map_record *record =
                    markdown_core_reference_create(parser->mem, parser->refmap, &label, resource);
                if (record) {
                    record->implicit = true;
                    record->source_key =
                        ((uint64_t)(uint32_t)heading->node->start_line << 32) | (uint32_t)heading->node->start_column;
                }
                heading->resource = record ? record->resource : NULL;
            }
        }
    }
    clear_inlines(&subj);
}

void markdown_core_finish_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    if (heading->pending) {
        finish_inlines(parser, heading->pending);
        parser->mem->free(heading->pending);
        heading->pending = NULL;
    }
}

void markdown_core_dispose_heading(markdown_core_heading_parse *heading) {
    if (heading->pending) {
        markdown_core_mem *mem = heading->pending->mem;
        clear_inlines(heading->pending);
        mem->free(heading->pending);
        heading->pending = NULL;
    }
}

// Parse zero or more space characters, including at most one newline.
static void spnl(subject *subj) {
    skip_spaces(subj);
    if (skip_line_end(subj)) {
        skip_spaces(subj);
    }
}

// Parse reference.  Assumes string begins with '[' character.
// Modify refmap if a reference is encountered.
// Return 0 if no reference found, otherwise position of subject
// after reference is parsed.
static bool reference_tail(subject *subj, markdown_core_attribute_parser *attributes, markdown_core_attributes *value) {
    bufsize_t before = subj->pos;
    spnl(subj);
    bufsize_t base = (bufsize_t)(subj->input.data - attributes->data);
    bufsize_t start = base + subj->pos;
    bufsize_t end = markdown_core_attributes_end(attributes, start);
    if (end) {
        subj->pos = end - base;
        skip_spaces(subj);
        if (skip_line_end(subj)) {
            return markdown_core_attributes_parse(attributes, start, value, &end) != 0;
        }
    }
    subj->pos = before;
    skip_spaces(subj);
    return skip_line_end(subj);
}

bufsize_t markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input,
                                               markdown_core_map *refmap, markdown_core_attribute_parser *attributes,
                                               uint64_t source_key) {
    subject subj;
    markdown_core_resource *resource;
    int lost = 0;
    markdown_core_attributes value = {0};

    markdown_core_chunk lab;
    markdown_core_chunk url;
    markdown_core_chunk title;
    const markdown_core_chunk absent_title = MARKDOWN_CORE_CHUNK_EMPTY;

    bufsize_t matchlen = 0;
    bufsize_t beforetitle;

    subject_from_buf(NULL, mem, -1, &subj, input, NULL);

    // parse label:
    if (!link_label(&subj, &lab) || lab.len == 0) {
        return 0;
    }
    // colon:
    if (peek_char(&subj) == ':') {
        advance(&subj);
    } else {
        return 0;
    }

    // parse link url:
    spnl(&subj);
    if ((matchlen = manual_scan_link_url(&subj.input, subj.pos, &url)) > -1) {
        subj.pos += matchlen;
    } else {
        return 0;
    }

    // parse optional link_title
    beforetitle = subj.pos;
    spnl(&subj);
    matchlen = subj.pos == beforetitle ? 0 : scan_link_title(&subj.input, subj.pos);
    if (matchlen) {
        title = markdown_core_chunk_dup(&subj.input, subj.pos, matchlen);
        subj.pos += matchlen;
    } else {
        subj.pos = beforetitle;
        // No title was written, so record that rather than an empty one.
        title = absent_title;
    }

    // parse final spaces and newline:
    if (!reference_tail(&subj, attributes, &value)) {
        if (matchlen) { // try rewinding before title
            subj.pos = beforetitle;
            if (!reference_tail(&subj, attributes, &value)) {
                return 0;
            }
            // The title candidate is un-read here: its bytes stay paragraph
            // text, and the definition has no title. `title` still held the
            // scanned chunk, which then went into the reference map -- so a
            // reference to this label resolved with a title the definition does
            // not have, and the same bytes were stated twice, once as prose and
            // once as a title.
            title = absent_title;
        } else {
            return 0;
        }
    }
    if (!refmap) {
        markdown_core_attributes_free(mem, &value);
        return subj.pos;
    }
    // The definition is consumed into the map, which owns its resource ONCE
    // and lends it to every occurrence that resolves to the label (M2). The
    // destination and title are cleaned here, the way a direct link's are, so
    // a resolved occurrence and a direct one state the same values.
    {
        markdown_core_chunk clean_url = markdown_core_clean_url(mem, &url, &lost);
        markdown_core_optional_chunk clean_title = markdown_core_clean_title(mem, &title, &lost);
        resource = lost ? NULL : markdown_core_resource_new(mem, clean_url, clean_title);
        if (!resource) {
            markdown_core_chunk_free(mem, &clean_url);
            markdown_core_optional_chunk_free(mem, &clean_title);
            lost = 1;
        }
    }
    if (resource) {
        resource->attributes = value;
        markdown_core_map_record *record = markdown_core_reference_create(mem, refmap, &lab, resource);
        if (record) {
            record->source_key = source_key;
        }
    } else {
        markdown_core_attributes_free(mem, &value);
    }
    if ((subj.oom || lost) && refmap) {
        refmap->oom = 1;
    }
    return subj.pos;
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

unsigned char markdown_core_inline_parser_peek_char(markdown_core_inline_parser *parser) { return peek_char(parser); }

unsigned char markdown_core_inline_parser_peek_at(markdown_core_inline_parser *parser, bufsize_t pos) {
    return peek_at(parser, pos);
}

int markdown_core_inline_parser_is_eof(markdown_core_inline_parser *parser) { return is_eof(parser); }

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

    while ((c = peek_char(parser)) && (*pred)(c)) {
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
        while (peek_at(parser, before_char_pos) >> 6 == 2 && before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(parser->input.data + before_char_pos, parser->pos - before_char_pos,
                                             &before_char);
        if (len == -1) {
            before_char = 10;
        }
    }

    while (peek_char(parser) == c && numdelims < max_delims) {
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
    node = make_literal(parser, MARKDOWN_CORE_NODE_TEXT, from, to,
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
