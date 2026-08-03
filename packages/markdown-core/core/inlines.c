#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "markdown_core_ctype.h"
#include "config.h"
#include "node.h"
#include "parser.h"
#include "references.h"
#include "markdown-core.h"
#include "houdini.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "extension.h"
#include "delimiter.h"

static const char *EMDASH = "\xE2\x80\x94";
static const char *ENDASH = "\xE2\x80\x93";
static const char *ELLIPSES = "\xE2\x80\xA6";
static const char *LEFTDOUBLEQUOTE = "\xE2\x80\x9C";
static const char *RIGHTDOUBLEQUOTE = "\xE2\x80\x9D";
static const char *LEFTSINGLEQUOTE = "\xE2\x80\x98";
static const char *RIGHTSINGLEQUOTE = "\xE2\x80\x99";

// Macros for creating various kinds of simple.
#define make_str(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)
#define make_code(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_CODE, sc, ec, s)
#define make_raw_html(subj, sc, ec, s) make_literal(subj, MARKDOWN_CORE_NODE_HTML, sc, ec, s)
#define make_line_break(mem) make_simple(mem, MARKDOWN_CORE_NODE_LINE_BREAK)
#define make_soft_break(mem) make_simple(mem, MARKDOWN_CORE_NODE_SOFT_BREAK)
#define make_emphasis(mem) make_simple(mem, MARKDOWN_CORE_NODE_EMPHASIS)
#define make_strong(mem) make_simple(mem, MARKDOWN_CORE_NODE_STRONG)

#define MAXBACKTICKS 80

typedef struct bracket {
    struct bracket *previous;
    markdown_core_node *inl_text;
    markdown_core_bufsize position;
    uint64_t claim_order;
    markdown_core_delimiter_mark delimiter_mark;
    bool image;
    bool active;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
} bracket;

typedef enum {
    INLINE_PHASE_SCAN = 0,
    INLINE_PHASE_REDUCE = 1,
} inline_phase;

#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)

typedef struct subject {
    markdown_core_mem *mem;
    markdown_core_chunk input;
    unsigned flags;
    int line;
    markdown_core_bufsize pos;
    int block_offset;
    int column_offset;
    markdown_core_map *refmap;
    markdown_core_delimiter_engine *delimiters;
    markdown_core_inline_attachment *active_attachment;
    inline_phase phase;
    uint64_t claim_clock;
    bracket *last_bracket;
    markdown_core_bufsize backticks[MAXBACKTICKS + 1];
    bool scanned_for_backticks;
    bool no_link_openers;
    /* Borrowed from the owning parser (or the immutable core defaults when
     * there is no parser, e.g. reference parsing). */
    const int8_t *special_chars;
    const int8_t *skip_chars;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    int oom;
    int internal_error;
} subject;

// "\r\n\\`&_*[]<!"
static const int8_t BASE_SPECIAL_CHARS[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// No emphasis-boundary skip characters by default; attached inline extensions
// add theirs to the parser-local copy.
static const int8_t BASE_SKIP_CHARS[256] = {0};

markdown_core_inline_config *markdown_core_inlines_new_config(markdown_core_mem *mem) {
    return markdown_core_inline_config_new(mem, BASE_SPECIAL_CHARS, BASE_SKIP_CHARS);
}

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static markdown_core_delimiter_result S_reduce_emph(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
);
static markdown_core_delimiter_result S_reduce_quote(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
);

enum {
    CORE_DELIMITER_STAR = 0,
    CORE_DELIMITER_UNDERSCORE = 1,
    CORE_DELIMITER_SINGLE_QUOTE = 2,
    CORE_DELIMITER_DOUBLE_QUOTE = 3,
};

static const markdown_core_delimiter_rule CORE_DELIMITER_RULES[MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT] = {
    {MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK, MARKDOWN_CORE_DELIMITER_REDUCE_RUN, 0, NULL},
    {MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK, MARKDOWN_CORE_DELIMITER_REDUCE_RUN, 0, NULL},
    {MARKDOWN_CORE_DELIMITER_PAIR_NEAREST, MARKDOWN_CORE_DELIMITER_REDUCE_ENDPOINTS, 0, NULL},
    {MARKDOWN_CORE_DELIMITER_PAIR_NEAREST, MARKDOWN_CORE_DELIMITER_REDUCE_ENDPOINTS, 0, NULL},
};

static const markdown_core_delimiter_binding CORE_DELIMITER_BINDINGS[MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT] = {
    {
        .rule = &CORE_DELIMITER_RULES[CORE_DELIMITER_STAR],
        .reduce = S_reduce_emph,
        .lane = CORE_DELIMITER_STAR,
        .local_kind = CORE_DELIMITER_STAR,
    },
    {
        .rule = &CORE_DELIMITER_RULES[CORE_DELIMITER_UNDERSCORE],
        .reduce = S_reduce_emph,
        .lane = CORE_DELIMITER_UNDERSCORE,
        .local_kind = CORE_DELIMITER_UNDERSCORE,
    },
    {
        .rule = &CORE_DELIMITER_RULES[CORE_DELIMITER_SINGLE_QUOTE],
        .reduce = S_reduce_quote,
        .lane = CORE_DELIMITER_SINGLE_QUOTE,
        .local_kind = CORE_DELIMITER_SINGLE_QUOTE,
    },
    {
        .rule = &CORE_DELIMITER_RULES[CORE_DELIMITER_DOUBLE_QUOTE],
        .reduce = S_reduce_quote,
        .lane = CORE_DELIMITER_DOUBLE_QUOTE,
        .local_kind = CORE_DELIMITER_DOUBLE_QUOTE,
    },
};

static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent, int options);
static int subject_has_failure(const markdown_core_parser *parser, const subject *subj);

static void subject_from_buf(
    markdown_core_parser *parser,
    markdown_core_mem *mem,
    int line_number,
    int block_offset,
    subject *e,
    markdown_core_chunk *buffer,
    markdown_core_map *refmap
);
static markdown_core_bufsize subject_find_special_char(subject *subj, int options);

// Create an inline with a literal string value.
static MARKDOWN_CORE_INLINE markdown_core_node *make_literal(
    subject *subj,
    markdown_core_node_type t,
    int start_column,
    int end_column,
    markdown_core_chunk s
) {
    markdown_core_node *e = (markdown_core_node *)subj->mem->calloc(subj->mem, 1, sizeof(*e));
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(subj->mem, &s);
        subj->oom = 1;
        return NULL;
    }
    markdown_core_strbuf_init(subj->mem, &e->content, 0);
    e->type = (uint16_t)t;
    e->as.literal = s;
    e->start_line = e->end_line = subj->line;
    // columns are 1 based.
    e->start_column = start_column + 1 + subj->column_offset + subj->block_offset;
    e->end_column = end_column + 1 + subj->column_offset + subj->block_offset;
    return e;
}

// Create an inline with no value.
static MARKDOWN_CORE_INLINE markdown_core_node *make_simple(markdown_core_mem *mem, markdown_core_node_type t) {
    markdown_core_node *e = (markdown_core_node *)mem->calloc(mem, 1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &e->content, 0);
    e->type = (uint16_t)t;
    return e;
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
static markdown_core_node *make_str_with_entities(
    subject *subj,
    int start_column,
    int end_column,
    markdown_core_chunk *content
) {
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT(subj->mem);

    if (markdown_core_houdini_unescape_html(&unescaped, content->data, content->len)) {
        if (unescaped.oom) {
            subj->oom = 1;
        }
        return make_str(subj, start_column, end_column, markdown_core_chunk_buf_detach(&unescaped));
    } else {
        return make_str(subj, start_column, end_column, *content);
    }
}

// Duplicate a chunk by creating a copy of the buffer not by reusing the
// buffer like markdown_core_chunk_borrow does.
static markdown_core_chunk chunk_clone(subject *subj, markdown_core_chunk *src) {
    markdown_core_chunk c;
    markdown_core_bufsize len = src->len;

    c.len = len;
    c.data = (unsigned char *)subj->mem->calloc(subj->mem, (size_t)len + 1, 1);
    if (!c.data) {
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        subj->oom = 1;
        return empty;
    }
    c.alloc = 1;
    if (len) {
        memcpy(c.data, src->data, len);
    }
    c.data[len] = '\0';

    return c;
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

    markdown_core_houdini_unescape_html_f(&buf, url->data, url->len);
    if (buf.oom) {
        subj->oom = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

static MARKDOWN_CORE_INLINE markdown_core_node *make_autolink(
    subject *subj,
    int start_column,
    int end_column,
    markdown_core_chunk url,
    int is_email
) {
    markdown_core_node *link = make_simple(subj->mem, MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *text;
    if (!link) {
        subj->oom = 1;
        return NULL;
    }
    link->as.link.url = markdown_core_clean_autolink(subj, &url, is_email);
    link->as.link.title = markdown_core_chunk_literal("");
    link->start_line = link->end_line = subj->line;
    link->start_column = start_column + 1;
    link->end_column = end_column + 1;
    text = make_str_with_entities(subj, start_column + 1, end_column - 1, &url);
    if (text) {
        markdown_core_node_append_child_unchecked(link, text);
    }
    return link;
}

static void subject_from_buf(
    markdown_core_parser *parser,
    markdown_core_mem *mem,
    int line_number,
    int block_offset,
    subject *e,
    markdown_core_chunk *chunk,
    markdown_core_map *refmap
) {
    int i;
    size_t extension_rule_count = parser && parser->inline_config ? parser->inline_config->extension_rule_count : 0;
    e->special_chars = parser && parser->inline_config ? parser->inline_config->special_chars : BASE_SPECIAL_CHARS;
    e->skip_chars = parser && parser->inline_config ? parser->inline_config->skip_chars : BASE_SKIP_CHARS;
    e->mem = mem;
    e->input = *chunk;
    e->flags = 0;
    e->line = line_number;
    e->pos = 0;
    e->block_offset = block_offset;
    e->column_offset = 0;
    e->refmap = refmap;
    e->oom = 0;
    e->internal_error = 0;
    e->delimiters = parser ? &parser->inline_delimiters : NULL;
    if (e->delimiters && markdown_core_delimiter_engine_begin(
                             e->delimiters,
                             MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT + extension_rule_count
                         ) != MARKDOWN_CORE_DELIMITER_OK) {
        e->internal_error = 1;
    }
    e->active_attachment = NULL;
    e->phase = INLINE_PHASE_SCAN;
    e->claim_clock = 0;
    e->last_bracket = NULL;
    for (i = 0; i <= MAXBACKTICKS; i++) {
        e->backticks[i] = 0;
    }
    e->scanned_for_backticks = false;
    e->no_link_openers = true;
}

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE unsigned char peek_char_n(subject *subj, markdown_core_bufsize n) {
    // NULL bytes should have been stripped out by now.  If they're
    // present, it's a programming error:
    assert(!(subj->pos + n < subj->input.len && subj->input.data[subj->pos + n] == 0));
    return (subj->pos + n < subj->input.len) ? subj->input.data[subj->pos + n] : 0;
}

static MARKDOWN_CORE_INLINE unsigned char peek_char(subject *subj) { return peek_char_n(subj, 0); }

static MARKDOWN_CORE_INLINE unsigned char peek_at(subject *subj, markdown_core_bufsize pos) {
    return subj->input.data[pos];
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
    markdown_core_bufsize startpos = subj->pos;
    markdown_core_bufsize len = 0;

    while ((c = peek_char(subj)) && (*f)(c)) {
        advance(subj);
        len++;
    }

    return markdown_core_chunk_borrow(&subj->input, startpos, len);
}

// Return the number of newlines in a given span of text in a subject.  If
// the number is greater than zero, also return the number of characters
// between the last newline and the end of the span in `since_newline`.
static int count_newlines(subject *subj, markdown_core_bufsize from, markdown_core_bufsize len, int *since_newline) {
    int nls = 0;
    int since_nl = 0;

    while (len--) {
        if (subj->input.data[from++] == '\n') {
            ++nls;
            since_nl = 0;
        } else {
            ++since_nl;
        }
    }

    if (!nls) {
        return 0;
    }

    *since_newline = since_nl;
    return nls;
}

static void subject_set_delimiter_failure(subject *subj, markdown_core_delimiter_result result) {
    if (result == MARKDOWN_CORE_DELIMITER_OOM) {
        subj->oom = 1;
    } else if (result != MARKDOWN_CORE_DELIMITER_OK) {
        subj->internal_error = 1;
    }
}

static uint64_t subject_next_claim_order(subject *subj) {
    if (subj->claim_clock == UINT64_MAX) {
        subj->internal_error = 1;
        return 0;
    }
    return subj->claim_clock + 1;
}

static int source_span_through(subject *subj, markdown_core_bufsize end, markdown_core_inline_source_span *span) {
    int newlines;
    int since_newline = 0;

    if (!subj || !span || subj->phase != INLINE_PHASE_SCAN || end <= subj->pos || end > subj->input.len) {
        if (subj) {
            subj->internal_error = 1;
        }
        return 0;
    }

    span->start_line = subj->line;
    span->start_column = subj->pos + 1 + subj->column_offset + subj->block_offset;
    newlines = count_newlines(subj, subj->pos, end - subj->pos, &since_newline);
    span->end_line = subj->line + newlines;
    span->end_column = newlines ? since_newline : span->start_column + (int)(end - subj->pos) - 1;
    return 1;
}

static void commit_source_span(subject *subj, markdown_core_bufsize end, const markdown_core_inline_source_span *span) {
    if (span->end_line != span->start_line) {
        subj->line = span->end_line;
        subj->column_offset = -end + span->end_column;
    }
    subj->pos = end;
}

static markdown_core_node *stage_source_text(
    subject *subj,
    markdown_core_bufsize start,
    markdown_core_bufsize end,
    const markdown_core_inline_source_span *span
) {
    markdown_core_node *node = make_simple_subj(subj, MARKDOWN_CORE_NODE_TEXT);
    if (!node) {
        return NULL;
    }
    node->as.literal = markdown_core_chunk_borrow(&subj->input, start, end - start);
    node->start_line = span->start_line;
    node->start_column = span->start_column;
    node->end_line = span->end_line;
    node->end_column = span->end_column;
    return node;
}

// Adjust `node`'s `end_line`, `end_column`, and `subj`'s `line` and
// `column_offset` according to the number of newlines in a just-matched span
// of text in `subj`.  Scope tracking is mandatory (canonical-ast.md), so this
// always runs; it was a render-era option in cmark.
static void adjust_subj_node_newlines(subject *subj, markdown_core_node *node, int matchlen, int extra) {
    int since_newline;
    int newlines = count_newlines(subj, subj->pos - matchlen - extra, matchlen, &since_newline);
    if (newlines) {
        subj->line += newlines;
        node->end_line += newlines;
        node->end_column = since_newline;
        subj->column_offset = -subj->pos + since_newline + extra;
    }
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
static markdown_core_bufsize scan_to_closing_backticks(subject *subj, markdown_core_bufsize openticklength) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array subj->backticks:
        return 0;
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
        markdown_core_bufsize numticks = 0;
        while (peek_char(subj) == '`') {
            advance(subj);
            numticks++;
        }
        // store position of ender
        if (numticks <= MAXBACKTICKS) {
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
    markdown_core_bufsize r, w;
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
static markdown_core_node *handle_backticks(subject *subj, int options) {
    markdown_core_chunk openticks = take_while(subj, isbacktick);
    markdown_core_bufsize startpos = subj->pos;
    markdown_core_bufsize endpos = scan_to_closing_backticks(subj, openticks.len);

    if (endpos == 0) {        // not found
        subj->pos = startpos; // rewind
        return make_str(subj, subj->pos, subj->pos, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

        markdown_core_strbuf_set(&buf, subj->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            subj->oom = 1;
        }

        markdown_core_node *node =
            make_code(subj, startpos, endpos - openticks.len - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        adjust_subj_node_newlines(subj, node, endpos - startpos, openticks.len);
        return node;
    }
}

// Scan ***, **, or * and return number scanned, or 0.
// Advances position.
static int scan_delims(subject *subj, unsigned char c, bool *can_open, bool *can_close) {
    int numdelims = 0;
    markdown_core_bufsize before_char_pos, after_char_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool left_flanking, right_flanking;

    if (subj->pos == 0) {
        before_char = 10;
    } else {
        before_char_pos = subj->pos - 1;
        // walk back to the beginning of the UTF_8 sequence:
        while ((peek_at(subj, before_char_pos) >> 6 == 2 || subj->skip_chars[peek_at(subj, before_char_pos)]) &&
               before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(
            subj->input.data + before_char_pos,
            subj->pos - before_char_pos,
            &before_char
        );
        if (len == -1 || (before_char < 256 && subj->skip_chars[(unsigned char)before_char])) {
            before_char = 10;
        }
    }

    if (c == '\'' || c == '"') {
        numdelims++;
        advance(subj); // limit to 1 delim for quotes
    } else {
        while (peek_char(subj) == c) {
            numdelims++;
            advance(subj);
        }
    }

    if (subj->pos == subj->input.len) {
        after_char = 10;
    } else {
        after_char_pos = subj->pos;
        while (after_char_pos < subj->input.len && subj->skip_chars[peek_at(subj, after_char_pos)]) {
            after_char_pos += 1;
        }
        len = markdown_core_utf8proc_iterate(
            subj->input.data + after_char_pos,
            subj->input.len - after_char_pos,
            &after_char
        );
        if (len == -1 || (after_char < 256 && subj->skip_chars[(unsigned char)after_char])) {
            after_char = 10;
        }
    }

    left_flanking =
        numdelims > 0 && !markdown_core_utf8proc_is_space(after_char) &&
        (!markdown_core_utf8proc_is_punctuation(after_char) || markdown_core_utf8proc_is_space(before_char) ||
         markdown_core_utf8proc_is_punctuation(before_char));
    right_flanking = numdelims > 0 && !markdown_core_utf8proc_is_space(before_char) &&
                     (!markdown_core_utf8proc_is_punctuation(before_char) ||
                      markdown_core_utf8proc_is_space(after_char) || markdown_core_utf8proc_is_punctuation(after_char));
    if (c == '_') {
        *can_open = left_flanking && (!right_flanking || markdown_core_utf8proc_is_punctuation(before_char));
        *can_close = right_flanking && (!left_flanking || markdown_core_utf8proc_is_punctuation(after_char));
    } else if (c == '\'' || c == '"') {
        *can_open = left_flanking && !right_flanking && before_char != ']' && before_char != ')';
        *can_close = right_flanking;
    } else {
        *can_open = left_flanking;
        *can_close = right_flanking;
    }
    return numdelims;
}

static void pop_bracket(subject *subj) {
    bracket *b;
    if (subj->last_bracket == NULL) {
        return;
    }
    b = subj->last_bracket;
    subj->last_bracket = subj->last_bracket->previous;
    subj->mem->free(subj->mem, b);
}

static const markdown_core_delimiter_binding *core_delimiter_binding(unsigned char c) {
    switch (c) {
    case '*':
        return &CORE_DELIMITER_BINDINGS[CORE_DELIMITER_STAR];
    case '_':
        return &CORE_DELIMITER_BINDINGS[CORE_DELIMITER_UNDERSCORE];
    case '\'':
        return &CORE_DELIMITER_BINDINGS[CORE_DELIMITER_SINGLE_QUOTE];
    case '"':
        return &CORE_DELIMITER_BINDINGS[CORE_DELIMITER_DOUBLE_QUOTE];
    default:
        return NULL;
    }
}

static void push_bracket(subject *subj, bool image, markdown_core_node *inl_text) {
    bracket *b = (bracket *)subj->mem->calloc(subj->mem, 1, sizeof(bracket));
    uint64_t claim_order;
    if (!b) {
        subj->oom = 1;
        return;
    }
    claim_order = subject_next_claim_order(subj);
    if (!claim_order) {
        subj->mem->free(subj->mem, b);
        return;
    }
    if (subj->last_bracket != NULL) {
        subj->last_bracket->bracket_after = true;
        b->in_bracket_image0 = subj->last_bracket->in_bracket_image0;
        b->in_bracket_image1 = subj->last_bracket->in_bracket_image1;
    }
    b->image = image;
    b->active = true;
    b->inl_text = inl_text;
    b->previous = subj->last_bracket;
    b->position = subj->pos;
    b->claim_order = claim_order;
    b->delimiter_mark = markdown_core_delimiter_engine_mark(subj->delimiters);
    b->bracket_after = false;
    if (image) {
        b->in_bracket_image1 = true;
    } else {
        b->in_bracket_image0 = true;
    }
    subj->last_bracket = b;
    subj->claim_clock = claim_order;
    if (!image) {
        subj->no_link_openers = false;
    }
}

// Assumes the subject has a c at the current position.
static markdown_core_node *handle_delim(subject *subj, unsigned char c, bool smart) {
    markdown_core_bufsize numdelims;
    markdown_core_node *inl_text;
    bool can_open, can_close;
    markdown_core_chunk contents;

    numdelims = scan_delims(subj, c, &can_open, &can_close);

    if (c == '\'' && smart) {
        contents = markdown_core_chunk_literal(RIGHTSINGLEQUOTE);
    } else if (c == '"' && smart) {
        contents = markdown_core_chunk_literal(can_close ? RIGHTDOUBLEQUOTE : LEFTDOUBLEQUOTE);
    } else {
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos - numdelims, numdelims);
    }

    inl_text = make_str(subj, subj->pos - numdelims, subj->pos - 1, contents);

    if (inl_text && (can_open || can_close) && (!(c == '\'' || c == '"') || smart)) {
        subject_set_delimiter_failure(
            subj,
            markdown_core_delimiter_engine_push(
                subj->delimiters,
                core_delimiter_binding(c),
                can_open,
                can_close,
                inl_text,
                subj->pos - numdelims,
                subj->pos,
                0
            )
        );
    }

    return inl_text;
}

// Assumes we have a hyphen at the current position.
static markdown_core_node *handle_hyphen(subject *subj, bool smart) {
    int startpos = subj->pos;

    advance(subj);

    if (!smart || peek_char(subj) != '-') {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("-"));
    }

    while (smart && peek_char(subj) == '-') {
        advance(subj);
    }

    int numhyphens = subj->pos - startpos;
    int en_count = 0;
    int em_count = 0;
    int i;
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

    if (numhyphens % 3 == 0) { // if divisible by 3, use all em dashes
        em_count = numhyphens / 3;
    } else if (numhyphens % 2 == 0) { // if divisible by 2, use all en dashes
        en_count = numhyphens / 2;
    } else if (numhyphens % 3 == 2) { // use one en dash at end
        en_count = 1;
        em_count = (numhyphens - 2) / 3;
    } else { // use two en dashes at the end
        en_count = 2;
        em_count = (numhyphens - 4) / 3;
    }

    for (i = em_count; i > 0; i--) {
        markdown_core_strbuf_puts(&buf, EMDASH);
    }

    for (i = en_count; i > 0; i--) {
        markdown_core_strbuf_puts(&buf, ENDASH);
    }

    if (buf.oom) {
        subj->oom = 1;
    }
    return make_str(subj, startpos, subj->pos - 1, markdown_core_chunk_buf_detach(&buf));
}

// Assumes we have a period at the current position.
static markdown_core_node *handle_period(subject *subj, bool smart) {
    advance(subj);
    if (smart && peek_char(subj) == '.') {
        advance(subj);
        if (peek_char(subj) == '.') {
            advance(subj);
            return make_str(subj, subj->pos - 3, subj->pos - 1, markdown_core_chunk_literal(ELLIPSES));
        } else {
            return make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_literal(".."));
        }
    } else {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("."));
    }
}

static void process_delimiters(markdown_core_parser *parser, subject *subj, markdown_core_delimiter_mark mark) {
    markdown_core_delimiter_result result;
    if (subj->phase != INLINE_PHASE_SCAN) {
        subj->internal_error = 1;
        return;
    }
    subj->phase = INLINE_PHASE_REDUCE;
    result = subject_has_failure(parser, subj)
                 ? markdown_core_delimiter_engine_truncate(subj->delimiters, mark)
                 : markdown_core_delimiter_engine_process(subj->delimiters, parser, subj, mark);
    subj->phase = INLINE_PHASE_SCAN;
    subject_set_delimiter_failure(subj, result);
}

static markdown_core_delimiter_result S_reduce_emph(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    subject *subj = inline_parser;
    markdown_core_node *opener_inl = match->opener_node;
    markdown_core_node *closer_inl = match->closer_node;
    markdown_core_bufsize opener_num_chars = match->opener_remaining - match->use_length;
    markdown_core_bufsize closer_num_chars = match->closer_remaining - match->use_length;
    markdown_core_node *tmp, *tmpnext, *emph;

    /* Allocate before changing either endpoint. An OOM parse is discarded,
     * but the local AST still remains internally consistent for cleanup. */
    emph = match->use_length == 1 ? make_emphasis(subj->mem) : make_strong(subj->mem);
    if (!emph) {
        return MARKDOWN_CORE_DELIMITER_OOM;
    }

    opener_inl->as.literal.len = opener_num_chars;
    closer_inl->as.literal.len = closer_num_chars;
    tmp = opener_inl->next;
    while (tmp && tmp != closer_inl) {
        tmpnext = tmp->next;
        markdown_core_node_append_child_unchecked(emph, tmp);
        tmp = tmpnext;
    }
    markdown_core_node_insert_after_unchecked(opener_inl, emph);

    emph->start_line = opener_inl->start_line;
    emph->end_line = closer_inl->end_line;
    emph->start_column = opener_inl->start_column;
    emph->end_column = closer_inl->end_column;

    if (opener_num_chars == 0) {
        markdown_core_node_free(opener_inl);
    }

    if (closer_num_chars == 0) {
        markdown_core_node_free(closer_inl);
    }
    return MARKDOWN_CORE_DELIMITER_OK;
}

static markdown_core_delimiter_result S_reduce_quote(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    subject *subj = inline_parser;
    const char *left = match->kind == CORE_DELIMITER_SINGLE_QUOTE ? LEFTSINGLEQUOTE : LEFTDOUBLEQUOTE;
    const char *right = match->kind == CORE_DELIMITER_SINGLE_QUOTE ? RIGHTSINGLEQUOTE : RIGHTDOUBLEQUOTE;
    markdown_core_chunk_free(subj->mem, &match->opener_node->as.literal);
    markdown_core_chunk_free(subj->mem, &match->closer_node->as.literal);
    match->opener_node->as.literal = markdown_core_chunk_literal(left);
    match->closer_node->as.literal = markdown_core_chunk_literal(right);
    return MARKDOWN_CORE_DELIMITER_OK;
}

// Parse backslash-escape or just a backslash, returning an inline.
static markdown_core_node *handle_backslash(markdown_core_parser *parser, subject *subj) {
    markdown_core_bufsize start = subj->pos;
    advance(subj);
    unsigned char nextchar = peek_char(subj);
    if (markdown_core_ispunct(nextchar)) {
        if (nextchar == '\\' && parser->inline_config->dispatch['\\'].count == 0) {
            markdown_core_bufsize end = start;
            while (end + 1 < subj->input.len && subj->input.data[end] == '\\' && subj->input.data[end + 1] == '\\') {
                end += 2;
            }
            /* Every complete pair decodes to one backslash. The first half
             * of an all-backslash source run is therefore already the exact
             * output bytes: borrow it while the node scope covers the full
             * consumed run. This is the same operation for one or many pairs
             * and requires no transformed-payload allocation. */
            subj->pos = end;
            return make_str(subj, start, end - 1, markdown_core_chunk_borrow(&subj->input, start, (end - start) / 2));
        }
        // only ascii symbols and newline can be escaped
        advance(subj);
        return make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_borrow(&subj->input, subj->pos - 1, 1));
    } else if (!is_eof(subj) && skip_line_end(subj)) {
        return make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("\\"));
    }
}

// Parse an entity or a regular "&" string.
// Assumes the subject has an '&' character at the current position.
static markdown_core_node *handle_entity(subject *subj) {
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT(subj->mem);
    markdown_core_bufsize len;

    advance(subj);

    len = markdown_core_houdini_unescape_ent(&ent, subj->input.data + subj->pos, subj->input.len - subj->pos);

    if (len == 0) {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("&"));
    }

    subj->pos += len;
    if (ent.oom) {
        subj->oom = 1;
    }
    return make_str(subj, subj->pos - 1 - len, subj->pos - 1, markdown_core_chunk_buf_detach(&ent));
}

// Clean a URL: remove surrounding whitespace, and remove \ that escape
// punctuation.
markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        markdown_core_chunk result = MARKDOWN_CORE_CHUNK_EMPTY;
        return result;
    }

    markdown_core_houdini_unescape_html_f(&buf, url->data, url->len);

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

markdown_core_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);
    unsigned char first, last;

    if (title->len == 0) {
        markdown_core_chunk result = MARKDOWN_CORE_CHUNK_EMPTY;
        return result;
    }

    first = title->data[0];
    last = title->data[title->len - 1];

    // remove surrounding quotes if any:
    if ((first == '\'' && last == '\'') || (first == '(' && last == ')') || (first == '"' && last == '"')) {
        markdown_core_houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
    } else {
        markdown_core_houdini_unescape_html_f(&buf, title->data, title->len);
    }

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

// Parse an autolink or HTML tag.
// Assumes the subject has a '<' character at the current position.
static markdown_core_node *handle_pointy_brace(subject *subj, int options) {
    markdown_core_bufsize matchlen = 0;
    markdown_core_chunk contents;

    advance(subj); // advance past first <

    // first try to match a URL autolink
    matchlen = scan_autolink_uri(&subj->input, subj->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos, matchlen - 1);
        subj->pos += matchlen;

        return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 0);
    }

    // next try to match an email autolink
    matchlen = scan_autolink_email(&subj->input, subj->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos, matchlen - 1);
        subj->pos += matchlen;

        return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 1);
    }

    // finally, try to match an html tag
    if (subj->pos + 2 <= subj->input.len) {
        int c = subj->input.data[subj->pos];
        if (c == '!' && (subj->flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = subj->input.data[subj->pos + 1];
            if (c == '-' && subj->input.data[subj->pos + 2] == '-') {
                if (subj->input.data[subj->pos + 3] == '>') {
                    matchlen = 4;
                } else if (subj->input.data[subj->pos + 3] == '-' && subj->input.data[subj->pos + 4] == '>') {
                    matchlen = 5;
                } else {
                    matchlen = scan_html_comment(&subj->input, subj->pos + 1);
                    if (matchlen > 0) {
                        matchlen += 1; // prefix "<"
                    } else {           // no match through end of input: set a flag so
                                       // we don't reparse looking for -->:
                        subj->flags |= FLAG_SKIP_HTML_COMMENT;
                    }
                }
            } else if (c == '[') {
                if ((subj->flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(&subj->input, subj->pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (subj->pos + matchlen > subj->input.len) {
                            subj->flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((subj->flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(&subj->input, subj->pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (subj->pos + matchlen > subj->input.len) {
                        subj->flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((subj->flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(&subj->input, subj->pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (subj->pos + matchlen > subj->input.len) {
                    subj->flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(&subj->input, subj->pos);
        }
    }
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos - 1, matchlen + 1);
        subj->pos += matchlen;
        markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        adjust_subj_node_newlines(subj, node, matchlen, 1);
        return node;
    }

    // if nothing matches, just return the opening <:
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("<"));
}

// Parse a link label.  Returns 1 if successful.
// Note:  unescaped brackets are not allowed in labels.
// The label begins with `[` and ends with the first `]` character
// encountered.  Backticks in labels do not start code spans.
static int link_label(subject *subj, markdown_core_chunk *raw_label) {
    markdown_core_bufsize startpos = subj->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (peek_char(subj) == '[') {
        advance(subj);
    } else {
        return 0;
    }

    while ((c = peek_char(subj)) && c != '[' && c != ']') {
        if (c == '\\') {
            advance(subj);
            length++;
            if (markdown_core_ispunct(peek_char(subj))) {
                advance(subj);
                length++;
            }
        } else {
            advance(subj);
            length++;
        }
        if (length > MAX_LINK_LABEL_LENGTH) {
            goto noMatch;
        }
    }

    if (c == ']') { // match found
        *raw_label = markdown_core_chunk_borrow(&subj->input, startpos + 1, subj->pos - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        advance(subj); // advance past ]
        return 1;
    }

noMatch:
    subj->pos = startpos; // rewind
    return 0;
}

static markdown_core_bufsize manual_scan_link_url_2(
    markdown_core_chunk *input,
    markdown_core_bufsize offset,
    markdown_core_chunk *output
) {
    markdown_core_bufsize i = offset;
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

static markdown_core_bufsize manual_scan_link_url(
    markdown_core_chunk *input,
    markdown_core_bufsize offset,
    markdown_core_chunk *output
) {
    markdown_core_bufsize i = offset;

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

/* The node a source `[^label]` becomes.
 *
 * A label the document defines is a footnote reference. A label nobody
 * defines is not an unresolved reference — it is the text the author typed,
 * and it is built here as one literal Text node.
 *
 * Two consequences worth stating, because both were reached the other way
 * first. The label is not reparsed: `[^~~x~~]` is the literal nine characters,
 * not a bracket around a strikethrough, which is why this synthesizes the text
 * instead of falling through to the ordinary "emit the `]` and let the
 * delimiter stack sort it out" tail. And the answer is a definedness question
 * asked of the whole document, so it belongs to the session's footnote map
 * rather than to the block being parsed. */
static markdown_core_node *make_footnote_reference_or_text(
    subject *subj,
    const markdown_core_chunk *literal,
    int label_span,
    bool defined
) {
    markdown_core_node *node;
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

    if (defined) {
        node = make_simple(subj->mem, MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE);
        if (node) {
            node->as.literal = markdown_core_chunk_borrow(literal, 1, label_span);
        }
        return node;
    }

    node = make_simple(subj->mem, MARKDOWN_CORE_NODE_TEXT);
    if (!node) {
        return NULL;
    }
    /* Owned, not borrowed: the brackets and the caret are not contiguous with
     * the label in any buffer the node could point into. */
    markdown_core_strbuf_puts(&buf, "[^");
    markdown_core_strbuf_put(&buf, literal->data + 1, label_span);
    markdown_core_strbuf_putc(&buf, ']');
    node->as.literal = markdown_core_chunk_buf_detach(&buf);
    if (!node->as.literal.data) {
        markdown_core_node_free(node);
        return NULL;
    }
    return node;
}

// Return a link, an image, or a literal close bracket.
static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, subject *subj) {
    markdown_core_bufsize initial_pos, after_link_text_pos;
    markdown_core_bufsize endurl, starttitle, endtitle, endall;
    markdown_core_bufsize sps, n;
    markdown_core_reference *ref = NULL;
    markdown_core_chunk url_chunk, title_chunk;
    markdown_core_chunk url, title;
    bracket *opener;
    markdown_core_node *inl;
    markdown_core_chunk raw_label;
    int found_label;
    markdown_core_node *tmp, *tmpnext;
    bool is_image;
    /* Set on the reference path only. `[a](/u)` writes its destination in
     * the source and keeps carrying it; a reference does not, so the two
     * produce different node kinds from this one `match` label. */
    bool is_reference = false;
    markdown_core_reference_type form = MARKDOWN_CORE_SHORTCUT_REFERENCE;
    markdown_core_chunk label = markdown_core_chunk_literal("");

    advance(subj); // advance past ]
    initial_pos = subj->pos;

    // get last [ or ![
    opener = subj->last_bracket;

    if (opener == NULL) {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }

    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    is_image = opener->image;

    if (!is_image && subj->no_link_openers) {
        // take delimiter off stack
        pop_bracket(subj);
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }

    after_link_text_pos = subj->pos;

    // First, look for an inline link.
    if (peek_char(subj) == '(' && ((sps = scan_spacechars(&subj->input, subj->pos + 1)) > -1) &&
        ((n = manual_scan_link_url(&subj->input, subj->pos + 1 + sps, &url_chunk)) > -1)) {

        // try to parse an explicit link:
        endurl = subj->pos + 1 + sps + n;
        starttitle = endurl + scan_spacechars(&subj->input, endurl);

        // ensure there are spaces btw url and title
        endtitle = (starttitle == endurl) ? starttitle : starttitle + scan_link_title(&subj->input, starttitle);

        endall = endtitle + scan_spacechars(&subj->input, endtitle);

        if (peek_at(subj, endall) == ')') {
            subj->pos = endall + 1;

            title_chunk = markdown_core_chunk_borrow(&subj->input, starttitle, endtitle - starttitle);
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
    if (found_label) {
        form = raw_label.len ? MARKDOWN_CORE_FULL_REFERENCE : MARKDOWN_CORE_COLLAPSED_REFERENCE;
    }
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        subj->pos = initial_pos;
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(subj->mem, &raw_label);
        raw_label = markdown_core_chunk_borrow(&subj->input, opener->position, initial_pos - opener->position - 1);
        found_label = true;
    }

    if (found_label) {
        ref = (markdown_core_reference *)markdown_core_map_lookup(subj->refmap, &raw_label);
        if (ref != NULL) {
            /* Kept for the node: the label as written, which is what the
             * source says. Its normalized form stays the map's. chunk_dup
             * borrows the block's content, which the node outlives. */
            label = raw_label;
            if (!markdown_core_chunk_to_cstr(subj->mem, &label)) {
                subj->oom = 1;
            }
            is_reference = true;
        } else {
            markdown_core_chunk_free(subj->mem, &raw_label);
        }
    }

    if (ref != NULL) { // found
        url = chunk_clone(subj, &ref->url);
        title = chunk_clone(subj, &ref->title);
        goto match;
    } else {
        goto noMatch;
    }

noMatch:
    // If we fall through to here, it means we didn't match a link.
    // What if we're a footnote link?
    if (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES && opener->inl_text->next &&
        opener->inl_text->next->type == MARKDOWN_CORE_NODE_TEXT) {

        markdown_core_chunk *literal = &opener->inl_text->next->as.literal;

        // look back to the opening '[', and skip ahead to the next character
        // if we're looking at a '[^' sequence, and there is other text or nodes
        // after the ^, let's call it a footnote reference.
        if ((literal->len > 0 && literal->data[0] == '^') && (literal->len > 1 || opener->inl_text->next->next)) {

            // A label with no non-whitespace character names nothing and can
            // never resolve; such brackets stay ordinary text. The label is
            // the raw bytes between "[^" and "]", read exactly like the
            // chunk_dup below with the position rewound to initial_pos (the
            // literal borrows the block's content buffer, so the bytes past
            // literal->len up to the label length are in bounds).
            int label_span =
                (initial_pos + subj->column_offset + subj->block_offset) - opener->inl_text->start_column - 2;
            bool label_blank = true;
            for (int i = 0; i < label_span && label_blank; i++) {
                label_blank = markdown_core_isspace(literal->data[1 + i]);
            }
            if (label_blank) {
                pop_bracket(subj);
                subj->pos = initial_pos;
                return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
            }

            // Whether the document defines the label decides which node this
            // is, not merely how a consumer resolves it. The three reference
            // forms answer "no definition" the same way — as text — and for
            // link and image references the syntax leaves no other option, so
            // a footnote reference that stayed a node would be the one place a
            // renderer had to special-case. The label reads exactly as the
            // borrow below reads it.
            //
            // The map is the session's, spanning the whole document: a
            // paragraph reparsed on its own still sees a definition a hundred
            // lines further down, so an incremental tree equals the one-shot
            // tree. A definedness flip is what re-refines the units that read
            // the label.
            markdown_core_chunk probe = {literal->data + 1, label_span, 0};
            bool defined = markdown_core_map_lookup(parser->footnote_defs, &probe) != NULL;

            // Before we got this far, the `handle_close_bracket` function may have
            // advanced the current state beyond our footnote's actual closing
            // bracket, ie if it went looking for a `link_label`.
            // Let's just rewind the subject's position:
            subj->pos = initial_pos;

            // the start and end of the footnote ref is the opening and closing brace
            // i.e. the subject's current position, and the opener's start_column
            int fnref_end_column = subj->pos + subj->column_offset + subj->block_offset;
            int fnref_start_column = opener->inl_text->start_column;

            // any given node delineates a substring of the line being processed,
            // with the remainder of the line being pointed to thru its 'literal'
            // struct member.
            // here, we copy the literal's pointer, moving it past the '^' character
            // for a length equal to the size of footnote reference text.
            // i.e. end_col minus start_col, minus the [ and the ^ characters
            //
            // this copies the footnote reference string, even if between the
            // `opener` and the subject's current position there are other nodes
            markdown_core_node *fnref = make_footnote_reference_or_text(subj, literal, label_span, defined);
            if (!fnref) {
                subj->oom = 1;
                pop_bracket(subj);
                return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
            }

            fnref->start_line = fnref->end_line = subj->line;
            fnref->start_column = fnref_start_column;
            fnref->end_column = fnref_end_column;

            // we then replace the opener with this new fnref node, the net effect
            // being replacing the opening '[' text node with a `^footnote-ref]` node.
            markdown_core_node_insert_before(opener->inl_text, fnref);

            process_delimiters(parser, subj, opener->delimiter_mark);
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
            markdown_core_node *next_node;
            markdown_core_node *current_node = opener->inl_text->next;
            while (current_node) {
                next_node = current_node->next;
                markdown_core_node_free(current_node);
                current_node = next_node;
            }

            markdown_core_node_free(opener->inl_text);

            pop_bracket(subj);
            return NULL;
        }
    }

    pop_bracket(subj); // remove this opener from delimiter list
    subj->pos = initial_pos;
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));

match:
    if (is_reference) {
        inl = make_simple(subj->mem, is_image ? MARKDOWN_CORE_NODE_IMAGE_REFERENCE : MARKDOWN_CORE_NODE_LINK_REFERENCE);
    } else {
        inl = make_simple(subj->mem, is_image ? MARKDOWN_CORE_NODE_IMAGE : MARKDOWN_CORE_NODE_LINK);
    }
    if (!inl) {
        subj->oom = 1;
        markdown_core_chunk_free(subj->mem, &url);
        markdown_core_chunk_free(subj->mem, &title);
        markdown_core_chunk_free(subj->mem, &label);
        pop_bracket(subj);
        subj->pos = initial_pos;
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }
    if (is_reference) {
        /* The destination is the definition's, stated once there. Resolving
         * it into every reference is what made a definition edit reparse
         * every unit that mentions the label. */
        inl->as.reference.label = label;
        inl->as.reference.form = form;
    } else {
        inl->as.link.url = url;
        inl->as.link.title = title;
    }
    inl->start_line = inl->end_line = subj->line;
    inl->start_column = opener->inl_text->start_column;
    inl->end_column = subj->pos + subj->column_offset + subj->block_offset;
    markdown_core_node_insert_before(opener->inl_text, inl);
    // Add link text:
    tmp = opener->inl_text->next;
    while (tmp) {
        tmpnext = tmp->next;
        markdown_core_node_append_child_unchecked(inl, tmp);
        tmp = tmpnext;
    }

    // Free the bracket [:
    markdown_core_node_free(opener->inl_text);

    process_delimiters(parser, subj, opener->delimiter_mark);
    pop_bracket(subj);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        subj->no_link_openers = true;
    }

    return NULL;
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the subject has a cr or newline at the current position.
static markdown_core_node *handle_newline(subject *subj) {
    markdown_core_bufsize nlpos = subj->pos;
    // skip over cr, crlf, or lf:
    if (peek_at(subj, subj->pos) == '\r') {
        advance(subj);
    }
    if (peek_at(subj, subj->pos) == '\n') {
        advance(subj);
    }
    ++subj->line;
    subj->column_offset = -subj->pos;
    // skip spaces at beginning of line
    skip_spaces(subj);
    if (nlpos > 1 && peek_at(subj, nlpos - 1) == ' ' && peek_at(subj, nlpos - 2) == ' ') {
        return make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        return make_simple_subj(subj, MARKDOWN_CORE_NODE_SOFT_BREAK);
    }
}

// " ' . -
static const char SMART_PUNCT_CHARS[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static markdown_core_bufsize subject_find_special_char(subject *subj, int options) {
    markdown_core_bufsize n = subj->pos + 1;

    while (n < subj->input.len) {
        if (subj->special_chars[subj->input.data[n]]) {
            return n;
        }
        if (options & MARKDOWN_CORE_OPT_SMART && SMART_PUNCT_CHARS[subj->input.data[n]]) {
            return n;
        }
        n++;
    }

    return subj->input.len;
}

static markdown_core_node *call_inline_attachment(
    markdown_core_inline_attachment *attachment,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char c,
    subject *subj
) {
    markdown_core_node *result;
    markdown_core_bufsize start = subj->pos;

    if (subj->phase != INLINE_PHASE_SCAN || subj->active_attachment) {
        subj->internal_error = 1;
        return NULL;
    }
    subj->active_attachment = attachment;
    result = attachment->extension->match_inline(attachment->extension, parser, parent, c, subj);
    subj->active_attachment = NULL;

    if (parser->oom) {
        subj->oom = 1;
    }
    if (parser->internal_error) {
        subj->internal_error = 1;
    }
    if ((result && (subj->pos <= start || subj->pos > subj->input.len)) || (!result && subj->pos != start)) {
        subj->internal_error = 1;
    }
    if ((subj->oom || subj->internal_error) && result) {
        markdown_core_node_free(result);
        result = NULL;
    }
    return result;
}

static markdown_core_node *try_extensions(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char c,
    subject *subj
) {
    const markdown_core_inline_dispatch *bucket = &parser->inline_config->dispatch[c];
    size_t i;
    for (i = 0; i < bucket->count; i++) {
        markdown_core_node *result = call_inline_attachment(bucket->items[i], parser, parent, c, subj);
        if (result || subj->oom || subj->internal_error) {
            return result;
        }
    }
    return NULL;
}

static int subject_has_failure(const markdown_core_parser *parser, const subject *subj) {
    return subj->oom || subj->internal_error || parser->oom || parser->internal_error;
}

typedef struct {
    markdown_core_node *node;
    int handled;
} shared_close_result;

static markdown_core_node *consume_rule_close(
    subject *subj,
    const markdown_core_delimiter_binding *binding,
    markdown_core_bufsize length
) {
    markdown_core_bufsize start = subj->pos;
    markdown_core_bufsize end;
    markdown_core_inline_source_span span;
    markdown_core_node *node;
    markdown_core_delimiter_result result;

    if (!binding || !binding->rule || !binding->rule->close_probe || length <= 0 || length > subj->input.len - start ||
        subj->input.data[start] != binding->rule->close_trigger) {
        subj->internal_error = 1;
        return NULL;
    }
    end = start + length;
    if (!source_span_through(subj, end, &span)) {
        return NULL;
    }
    node = stage_source_text(subj, start, end, &span);
    if (!node) {
        return NULL;
    }
    result = markdown_core_delimiter_engine_push(subj->delimiters, binding, 0, 1, node, start, end, 0);
    if (result != MARKDOWN_CORE_DELIMITER_OK) {
        markdown_core_node_free(node);
        subject_set_delimiter_failure(subj, result);
        return NULL;
    }
    commit_source_span(subj, end, &span);
    return node;
}

/*
 * A shared close belongs to the newest semantic opener whose pure lexical
 * probe accepts the current bytes. Every rule is probed at most once; only
 * the winning binding can consume, allocate, or mutate delimiter topology.
 */
static shared_close_result handle_shared_close(markdown_core_parser *parser, subject *subj, unsigned char trigger) {
    shared_close_result result = {NULL, 0};
    const markdown_core_inline_close_dispatch *bucket = &parser->inline_config->close_dispatch[trigger];
    const markdown_core_delimiter_binding *winner = NULL;
    markdown_core_bufsize winner_length = 0;
    uint64_t winner_order = 0;
    int bracket_wins = 0;
    size_t i;

    for (i = 0; i < bucket->count; i++) {
        const markdown_core_delimiter_binding *binding = bucket->items[i];
        markdown_core_delimiter_id opener = markdown_core_delimiter_engine_last_open(subj->delimiters, binding);
        markdown_core_bufsize length;
        uint64_t order;
        if (!opener) {
            continue;
        }
        order = markdown_core_delimiter_engine_claim_order(subj->delimiters, opener);
        if (!order) {
            subj->internal_error = 1;
            result.handled = 1;
            return result;
        }
        length = binding->rule->close_probe(binding->local_kind, subj->input.data, subj->input.len, subj->pos);
        if (length < 0 || length > subj->input.len - subj->pos) {
            subj->internal_error = 1;
            result.handled = 1;
            return result;
        }
        if (length > 0) {
            if (order == winner_order) {
                subj->internal_error = 1;
                result.handled = 1;
                return result;
            }
            if (order > winner_order) {
                winner = binding;
                winner_length = length;
                winner_order = order;
                bracket_wins = 0;
            }
        }
    }

    if (trigger == ']' && subj->last_bracket) {
        if (subj->last_bracket->claim_order == winner_order) {
            subj->internal_error = 1;
            result.handled = 1;
            return result;
        }
        if (subj->last_bracket->claim_order > winner_order) {
            winner = NULL;
            winner_order = subj->last_bracket->claim_order;
            bracket_wins = 1;
        }
    }
    if (!winner_order) {
        return result;
    }

    result.handled = 1;
    if (bracket_wins) {
        markdown_core_bufsize before = subj->pos;
        result.node = handle_close_bracket(parser, subj);
        if (!subj->oom && !subj->internal_error && (subj->pos <= before || subj->pos > subj->input.len)) {
            subj->internal_error = 1;
        }
        return result;
    }

    result.node = consume_rule_close(subj, winner, winner_length);
    return result;
}

// Parse an inline, advancing subject, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent, int options) {
    markdown_core_node *new_inl = NULL;
    markdown_core_chunk contents;
    shared_close_result shared_close;
    unsigned char c;
    markdown_core_bufsize startpos, endpos;
    c = peek_char(subj);
    if (c == 0) {
        return 0;
    }
    if (parser->inline_config->close_dispatch[c].count || (c == ']' && subj->last_bracket)) {
        shared_close = handle_shared_close(parser, subj, c);
        if (shared_close.handled) {
            new_inl = shared_close.node;
            goto parsed;
        }
    }
    switch (c) {
    case '\r':
    case '\n':
        new_inl = handle_newline(subj);
        break;
    case '`':
        new_inl = handle_backticks(subj, options);
        break;
    case '\\':
        new_inl = try_extensions(parser, parent, c, subj);
        if (!new_inl && subject_has_failure(parser, subj)) {
            goto parsed;
        }
        if (new_inl == NULL) {
            new_inl = handle_backslash(parser, subj);
        }
        break;
    case '&':
        new_inl = handle_entity(subj);
        break;
    case '<':
        new_inl = handle_pointy_brace(subj, options);
        break;
    case '*':
    case '_':
    case '\'':
    case '"':
        new_inl = handle_delim(subj, c, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '-':
        new_inl = handle_hyphen(subj, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '.':
        new_inl = handle_period(subj, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '[':
        new_inl = try_extensions(parser, parent, c, subj);
        if (!new_inl && subject_has_failure(parser, subj)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }
        advance(subj);
        new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("["));
        if (new_inl) {
            push_bracket(subj, false, new_inl);
        }
        break;
    case ']':
        new_inl = try_extensions(parser, parent, c, subj);
        if (!new_inl && !subject_has_failure(parser, subj)) {
            new_inl = handle_close_bracket(parser, subj);
        }
        break;
    case '!':
        new_inl = try_extensions(parser, parent, c, subj);
        if (!new_inl && subject_has_failure(parser, subj)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }

        advance(subj);
        if (peek_char(subj) == '[' && peek_char_n(subj, 1) != '^') {
            advance(subj);
            new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_literal("!["));
            if (new_inl) {
                push_bracket(subj, true, new_inl);
            }
        } else {
            new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("!"));
        }
        break;
    default:
        new_inl = try_extensions(parser, parent, c, subj);
        if (!new_inl && subject_has_failure(parser, subj)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }

        endpos = subject_find_special_char(subj, options);
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos, endpos - subj->pos);
        startpos = subj->pos;
        subj->pos = endpos;

        // if we're at a newline, strip trailing spaces.
        if (S_is_line_end_char(peek_char(subj))) {
            markdown_core_chunk_rtrim(&contents);
        }

        new_inl = make_str(subj, startpos, endpos - 1, contents);
    }
parsed:
    if (subject_has_failure(parser, subj)) {
        if (new_inl) {
            markdown_core_node_free(new_inl);
        }
        return 0;
    }
    if (new_inl != NULL) {
        markdown_core_node_append_child_unchecked(parent, new_inl);
    }

    return 1;
}

// Parse inlines from parent's string_content, adding as children of parent.
/* Longest line-aligned common prefix of two content buffers that is also
 * inert for inline parsing: no special character (the parser's table, which
 * includes every attached extension's) and, under SMART, no smart
 * punctuation; '\n' and '\r' delimit lines rather than disqualifying. Such
 * a prefix parses to exactly one Text and one break per line, and nothing at
 * or after the returned offset can pair with, or reshape, anything before it
 * — every pairing construct (emphasis, code spans, links, images, smart
 * quotes) needs an opener, and the prefix admits none. Returns 0 when no
 * usable seam exists; a nonzero seam always leaves a nonempty suffix on both
 * buffers. */
markdown_core_bufsize markdown_core_inline_seam_prefix(
    const markdown_core_parser *parser,
    const unsigned char *a,
    markdown_core_bufsize a_len,
    const unsigned char *b,
    markdown_core_bufsize b_len,
    int options
) {
    markdown_core_bufsize limit = a_len < b_len ? a_len : b_len;
    markdown_core_bufsize i = 0;
    markdown_core_bufsize seam = 0;
    while (i < limit) {
        unsigned char c = a[i];
        if (c != b[i]) {
            break;
        }
        if (c == '\n') {
            seam = i + 1;
        } else if (c == '\r') {
            // A carriage return is a line ending of its own (lone or as
            // CRLF), so it would break the one-Text-one-break-per-'\n'
            // accounting the transplant relies on; end the seam before it.
            break;
        } else {
            const markdown_core_inline_dispatch *seam_bucket = &parser->inline_config->seam_dispatch[c];
            int seam_barrier = parser->inline_config->seam_barrier_chars[c] != 0;
            size_t j;
            for (j = 0; !seam_barrier && j < seam_bucket->count; j++) {
                markdown_core_extension *extension = seam_bucket->items[j]->extension;
                seam_barrier = extension->inline_seam_probe(a, limit, i) != 0;
            }
            if (seam_barrier) {
                break;
            }
            if ((options & MARKDOWN_CORE_OPT_SMART) && SMART_PUNCT_CHARS[c]) {
                break;
            }
        }
        i++;
    }
    if (seam >= a_len || seam >= b_len) {
        return 0;
    }
    return seam;
}

void markdown_core_parse_inlines(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options
) {
    markdown_core_parse_inlines_from(parser, parent, refmap, options, 0);
}

void markdown_core_parse_inlines_from(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options,
    markdown_core_bufsize start
) {
    subject subj;
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    subject_from_buf(
        parser,
        parser->mem,
        parent->start_line,
        parent->start_column - 1 + parent->internal_offset,
        &subj,
        &content,
        refmap
    );
    markdown_core_chunk_rtrim(&subj.input);

    // Fast-forward over a caller-guaranteed inert prefix: same position
    // bookkeeping a real scan would leave (column = pos + 1 + column_offset
    // + block_offset; every newline resets column_offset to -pos and
    // advances the line).
    if (start > 0) {
        markdown_core_bufsize i;
        markdown_core_bufsize bound = start < subj.input.len ? start : subj.input.len;
        for (i = 0; i < start && i < content.len; i++) {
            if (content.data[i] == '\n') {
                subj.line++;
            }
        }
        // A seam at or past the rtrimmed end leaves nothing to parse; the
        // clamp keeps is_eof true instead of rescanning from zero.
        subj.pos = bound;
        subj.column_offset = -(int)start;
    }

    while (!is_eof(&subj) && parse_inline(parser, &subj, parent, options))
        ;

    process_delimiters(parser, &subj, (markdown_core_delimiter_mark){0, 0});
    // free bracket stack
    while (subj.last_bracket) {
        pop_bracket(&subj);
    }

    if (subj.oom) {
        parser->oom = true;
    }
    if (subj.internal_error) {
        parser->internal_error = true;
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
markdown_core_bufsize markdown_core_parse_reference_inline(
    markdown_core_mem *mem,
    markdown_core_chunk *input,
    markdown_core_map *refmap
) {
    subject subj;

    markdown_core_chunk lab;
    markdown_core_chunk url;
    markdown_core_chunk title;

    markdown_core_bufsize matchlen = 0;
    markdown_core_bufsize beforetitle;

    subject_from_buf(NULL, mem, -1, 0, &subj, input, NULL);

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
        title = markdown_core_chunk_borrow(&subj.input, subj.pos, matchlen);
        subj.pos += matchlen;
    } else {
        subj.pos = beforetitle;
        title = markdown_core_chunk_literal("");
    }

    // parse final spaces and newline:
    skip_spaces(&subj);
    if (!skip_line_end(&subj)) {
        if (matchlen) { // try rewinding before title
            subj.pos = beforetitle;
            skip_spaces(&subj);
            if (!skip_line_end(&subj)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    // insert reference into refmap
    markdown_core_reference_create(refmap, &lab, &url, &title);
    if (subj.oom && refmap) {
        refmap->oom = 1;
    }
    return subj.pos;
}

int markdown_core_inline_parser_consume_source(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize end_offset,
    markdown_core_inline_source_span *span
) {
    if (!parser || !parser->active_attachment || !source_span_through(parser, end_offset, span)) {
        if (parser) {
            parser->internal_error = 1;
        }
        return 0;
    }
    commit_source_span(parser, end_offset, span);
    return 1;
}

markdown_core_node *markdown_core_inline_parser_consume_text(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize end_offset
) {
    markdown_core_inline_source_span span;
    markdown_core_node *node;
    markdown_core_bufsize start;

    if (!parser || !parser->active_attachment || !source_span_through(parser, end_offset, &span)) {
        if (parser) {
            parser->internal_error = 1;
        }
        return NULL;
    }
    start = parser->pos;
    node = stage_source_text(parser, start, end_offset, &span);
    if (!node) {
        return NULL;
    }
    commit_source_span(parser, end_offset, &span);
    return node;
}

markdown_core_node *markdown_core_inline_parser_consume_delimiter(
    markdown_core_inline_parser *parser,
    uint16_t kind,
    int can_open,
    int can_close,
    markdown_core_bufsize end_offset
) {
    const markdown_core_delimiter_binding *binding;
    markdown_core_inline_source_span span;
    markdown_core_node *node;
    markdown_core_bufsize start;
    markdown_core_delimiter_result result;
    uint64_t claim_order = 0;

    if (!parser || !parser->active_attachment || kind >= parser->active_attachment->rule_count ||
        (!can_open && !can_close)) {
        if (parser) {
            parser->internal_error = 1;
        }
        return NULL;
    }
    binding = &parser->active_attachment->rules[kind];
    if (binding->rule->close_probe) {
        if (!can_open || can_close) {
            parser->internal_error = 1;
            return NULL;
        }
        claim_order = subject_next_claim_order(parser);
        if (!claim_order) {
            return NULL;
        }
    }
    if (!source_span_through(parser, end_offset, &span)) {
        return NULL;
    }
    start = parser->pos;
    node = stage_source_text(parser, start, end_offset, &span);
    if (!node) {
        return NULL;
    }
    result = markdown_core_delimiter_engine_push(
        parser->delimiters,
        binding,
        can_open,
        can_close,
        node,
        start,
        end_offset,
        claim_order
    );
    if (result != MARKDOWN_CORE_DELIMITER_OK) {
        markdown_core_node_free(node);
        subject_set_delimiter_failure(parser, result);
        return NULL;
    }
    commit_source_span(parser, end_offset, &span);
    if (claim_order) {
        parser->claim_clock = claim_order;
    }
    return node;
}

int markdown_core_inline_parser_scan_delimiters(
    markdown_core_inline_parser *parser,
    int max_delims,
    unsigned char c,
    int *left_flanking,
    int *right_flanking,
    int *punct_before,
    int *punct_after
) {
    int numdelims = 0;
    markdown_core_bufsize before_char_pos;
    markdown_core_bufsize scan_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool space_before, space_after;

    if (!parser || !parser->active_attachment || parser->phase != INLINE_PHASE_SCAN || max_delims <= 0 ||
        !left_flanking || !right_flanking || !punct_before || !punct_after) {
        if (parser) {
            parser->internal_error = 1;
        }
        return 0;
    }

    if (parser->pos == 0) {
        before_char = 10;
    } else {
        before_char_pos = parser->pos - 1;
        // walk back to the beginning of the UTF_8 sequence:
        while (peek_at(parser, before_char_pos) >> 6 == 2 && before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(
            parser->input.data + before_char_pos,
            parser->pos - before_char_pos,
            &before_char
        );
        if (len == -1) {
            before_char = 10;
        }
    }

    scan_pos = parser->pos;
    while (scan_pos < parser->input.len && parser->input.data[scan_pos] == c && numdelims < max_delims) {
        numdelims++;
        scan_pos++;
    }

    len = markdown_core_utf8proc_iterate(parser->input.data + scan_pos, parser->input.len - scan_pos, &after_char);
    if (len == -1 || scan_pos == parser->input.len) {
        after_char = 10;
    }

    *punct_before = markdown_core_utf8proc_is_punctuation(before_char);
    *punct_after = markdown_core_utf8proc_is_punctuation(after_char);
    space_before = markdown_core_utf8proc_is_space(before_char) != 0;
    space_after = markdown_core_utf8proc_is_space(after_char) != 0;

    *left_flanking = numdelims > 0 && !markdown_core_utf8proc_is_space(after_char) &&
                     !(*punct_after && !space_before && !*punct_before);
    *right_flanking = numdelims > 0 && !markdown_core_utf8proc_is_space(before_char) &&
                      !(*punct_before && !space_after && !*punct_after);

    return numdelims;
}

int markdown_core_inline_parser_get_offset(markdown_core_inline_parser *parser) { return parser->pos; }

int markdown_core_inline_parser_get_column(markdown_core_inline_parser *parser) {
    return parser->pos + 1 + parser->column_offset + parser->block_offset;
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

static void S_update_text_sourcepos(markdown_core_node *node) {
    if (node->start_line == 0) {
        return;
    }

    if (node->as.literal.len == 0) {
        node->start_line = 0;
        node->start_column = 0;
        node->end_line = 0;
        node->end_column = 0;
        return;
    }

    int end_line = node->start_line;
    int end_column = node->start_column - 1;
    for (markdown_core_bufsize i = 0; i < node->as.literal.len; i++) {
        if (node->as.literal.data[i] == '\n') {
            end_line++;
            end_column = 0;
        } else {
            end_column++;
        }
    }

    node->end_line = end_line;
    node->end_column = end_column;
}

void markdown_core_node_unput(markdown_core_node *node, int n) {
    node = node->last_child;
    while (n > 0 && node && node->type == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_bufsize remove =
            node->as.literal.len < (markdown_core_bufsize)n ? node->as.literal.len : (markdown_core_bufsize)n;
        node->as.literal.len -= remove;
        n -= (int)remove;
        S_update_text_sourcepos(node);
        node = node->prev;
    }
}

int markdown_core_inline_parser_get_line(markdown_core_inline_parser *parser) { return parser->line; }
