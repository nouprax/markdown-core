#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <assert.h>

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
#include "concrete_records.h"

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
    /* Capture index of this bracket's BRACKET_OPEN record: the patch key
     * when the bracket matches, and the retraction floor when a footnote
     * reference reinterprets everything from the opener on as one atomic
     * label. */
    size_t concrete_floor;
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
    /* The unit's inline concrete records under construction
     * (concrete_records.h): every capture site appends through it, the
     * engine patches reduce-time consumption into it, and the parse hands
     * it to the parsed node on success or abandons it with the parse.
     * Inert (mem NULL) exactly when there is no parser — reference
     * parsing dispatches no capturing handler. */
    markdown_core_concrete_capture capture;
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
static void subject_set_delimiter_failure(subject *subj, markdown_core_delimiter_result result);

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

static markdown_core_chunk markdown_core_clean_autolink(subject *subj, markdown_core_chunk *url, int is_email) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(subj->mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        /* No autolink the scanner accepts is empty — `<>` and `<a>` are not
         * autolinks at all — so this is unreachable today. It returns the
         * empty STRING rather than the empty chunk anyway, so that a link's
         * destination is non-NULL by construction on every path and not by
         * luck: the bindings type it as non-optional on that guarantee. */
        return markdown_core_chunk_literal("");
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
    /* No title, and the node is calloc'd, so the field is already the empty
     * chunk. Left unset deliberately: an autolink has nowhere to write a
     * title, and a NULL chunk is how the tree says "not written" — the same
     * distinction `[t](/u "")` and `[t](/u)` already carry. Assigning the ""
     * literal here reported a title the author never wrote. */
    link->start_line = link->end_line = subj->line;
    /* The two offsets are not optional here, however long they were
     * missing. A content offset is a position in the buffer the inline
     * pass scans; a column is a position on a source line, and the two
     * agree only while the buffer holds exactly one line that starts at
     * the margin. Without them an autolink on the second line of a
     * paragraph came out at `2:18-2:38` around its own child text at
     * `2:15-2:33` — a parent starting after its child ends, off by every
     * byte of every line above it. make_literal, which builds that very
     * child two lines below, has always added them; this is the same
     * function disagreeing with itself. Inherited from cmark-gfm
     * (src/inlines.c:178-181), which reproduces the numbers exactly, and
     * neither parity oracle compares positions, so nothing caught it. */
    link->start_column = start_column + 1 + subj->column_offset + subj->block_offset;
    link->end_column = end_column + 1 + subj->column_offset + subj->block_offset;
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
    markdown_core_concrete_capture_init(&e->capture, parser ? mem : NULL);
    e->delimiters = parser ? &parser->inline_delimiters : NULL;
    if (e->delimiters) {
        /* begin allocates the unit's lane table, so its failure can be an
         * allocation loss: classify through the one delimiter-result
         * funnel rather than reporting OOM as a broken invariant. */
        subject_set_delimiter_failure(
            e,
            markdown_core_delimiter_engine_begin(
                e->delimiters,
                MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT + extension_rule_count,
                &e->capture
            )
        );
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

/* Appends one inline concrete record for the token the calling handler just
 * consumed. Every caller runs under parse_inline, so the capture is always
 * engaged; a lost record joins the subject's sticky failure and the parse
 * is discarded rather than published thinner (the OOM sweep's property). */
static void capture_token(
    subject *subj,
    uint8_t kind,
    markdown_core_bufsize start,
    markdown_core_bufsize length,
    markdown_core_bufsize consumed
) {
    if (!markdown_core_concrete_capture_append(
            &subj->capture,
            kind,
            (uint32_t)start,
            (uint32_t)length,
            (uint32_t)consumed,
            0
        )) {
        subj->oom = 1;
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

        /* Both tick runs vanish from the projection (the Code literal is
         * the normalized interior), so both are markup material; the
         * matched closer's run length equals the opener's. */
        capture_token(
            subj,
            MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS,
            startpos - openticks.len,
            openticks.len,
            openticks.len
        );
        capture_token(
            subj,
            MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS,
            endpos - openticks.len,
            openticks.len,
            openticks.len
        );

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
    /* The opener's candidate record: consumed only if this bracket
     * matches, retracted wholesale if a footnote reference swallows it.
     * Its index doubles as both the patch key and the retraction floor. */
    b->concrete_floor = markdown_core_concrete_capture_count(&subj->capture);
    capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, subj->pos - (image ? 2 : 1), image ? 2 : 1, 0);
    if (subj->oom) {
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
        markdown_core_delimiter_result result = markdown_core_delimiter_engine_push(
            subj->delimiters,
            core_delimiter_binding(c),
            can_open,
            can_close,
            inl_text,
            subj->pos - numdelims,
            subj->pos,
            0
        );
        subject_set_delimiter_failure(subj, result);
        /* A smart quote's source byte was already replaced by its curly
         * glyph above, so the token is fully consumed whether or not it
         * later pairs; rewrite the push's generic run record to say so. */
        if (result == MARKDOWN_CORE_DELIMITER_OK && smart && (c == '\'' || c == '"')) {
            size_t index = markdown_core_concrete_capture_count(&subj->capture) - 1;
            markdown_core_concrete_capture_set_kind(&subj->capture, index, MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE);
            markdown_core_concrete_capture_consume_all(&subj->capture, index);
        }
    } else if (inl_text && smart && (c == '\'' || c == '"')) {
        /* Replaced but not flanking: no delimiter push happens, yet the
         * spelling is gone all the same. */
        capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, subj->pos - numdelims, numdelims, numdelims);
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
    /* A run of two or more was rewritten into dashes; a lone hyphen keeps
     * its own byte and records nothing (the early return above). */
    capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH, startpos, numhyphens, numhyphens);
    return make_str(subj, startpos, subj->pos - 1, markdown_core_chunk_buf_detach(&buf));
}

// Assumes we have a period at the current position.
static markdown_core_node *handle_period(subject *subj, bool smart) {
    advance(subj);
    if (smart && peek_char(subj) == '.') {
        advance(subj);
        if (peek_char(subj) == '.') {
            advance(subj);
            /* Exactly `...` became an ellipsis; `..` and `.` keep their
             * bytes below and record nothing. */
            capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS, subj->pos - 3, 3, 3);
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
            markdown_core_bufsize pair;
            while (end + 1 < subj->input.len && subj->input.data[end] == '\\' && subj->input.data[end + 1] == '\\') {
                end += 2;
            }
            /* Every complete pair decodes to one backslash. The first half
             * of an all-backslash source run is therefore already the exact
             * output bytes: borrow it while the node scope covers the full
             * consumed run. This is the same operation for one or many pairs
             * and requires no transformed-payload allocation. */
            /* The grammar's assignment, not the borrow trick's: each pair's
             * first backslash is the escape — one record per pair, the same
             * records the one-pair-at-a-time path below emits when an
             * extension owns the '\\' dispatch. */
            for (pair = start; pair < end; pair += 2) {
                capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, pair, 1, 1);
            }
            subj->pos = end;
            return make_str(subj, start, end - 1, markdown_core_chunk_borrow(&subj->input, start, (end - start) / 2));
        }
        // only ascii symbols and newline can be escaped
        advance(subj);
        capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, start, 1, 1);
        return make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_borrow(&subj->input, subj->pos - 1, 1));
    } else if (!is_eof(subj) && skip_line_end(subj)) {
        capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, start, 1, 1);
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
    /* The full spelling, `&` through `;`, decoded away into the text. */
    capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_ENTITY, subj->pos - 1 - len, len + 1, len + 1);
    return make_str(subj, subj->pos - 1 - len, subj->pos - 1, markdown_core_chunk_buf_detach(&ent));
}

// Clean a URL: remove surrounding whitespace, and remove \ that escape
// punctuation.
markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        /* Written and empty, not absent. Both callers reach here only with a
         * destination the author wrote — `[t]()`, `[t](<>)`, or a
         * definition's `<>` — so this is the empty STRING. NULL is reserved
         * for "not written", which is what lets a consumer tell `[t](/u "")`
         * from `[t](/u)`; a destination has no unwritten case. */
        return markdown_core_chunk_literal("");
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

        /* The whole `<...>` construct: brackets consumed, interior decoded
         * into the link's url and text. Raw HTML below stays recordless —
         * its literal is the exact source bytes. */
        capture_token(
            subj,
            MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK,
            subj->pos - 1 - matchlen,
            matchlen + 1,
            matchlen + 1
        );
        return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 0);
    }

    // next try to match an email autolink
    matchlen = scan_autolink_email(&subj->input, subj->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&subj->input, subj->pos, matchlen - 1);
        subj->pos += matchlen;

        capture_token(
            subj,
            MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK,
            subj->pos - 1 - matchlen,
            matchlen + 1,
            matchlen + 1
        );
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
        /* Raw HTML keeps its exact source bytes as its literal and records
         * nothing — comments included: a comment is an ordinary HTML node
         * the consumer classifies through the facade's comment bit, never
         * a deletion. */
        markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        if (!node) {
            return NULL;
        }
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
 * asked of the whole document, so it belongs to the document's footnote map
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
/* A unit's inline parse asks the definition tables; what it asked is
 * remembered on the parser until the unit's parse ends, then on the unit
 * (node.h `probes`), so a definition that arrives later finds exactly the
 * units whose answer it changes. A hash of zero is a label that could never
 * match and is not remembered; a lost allocation loses the probe, and the
 * parser's sticky bit says so, since a forgotten probe would be a unit a
 * later definition silently fails to reach. */
static void S_record_probe(markdown_core_parser *parser, uint64_t hash) {
    if (!parser || hash == 0) {
        return;
    }
    if (parser->probe_count == parser->probe_capacity) {
        size_t capacity = parser->probe_capacity ? parser->probe_capacity * 2 : 4;
        uint64_t *grown =
            (uint64_t *)parser->mem->realloc(parser->mem, parser->probe_hashes, capacity * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->probe_hashes = grown;
        parser->probe_capacity = capacity;
    }
    parser->probe_hashes[parser->probe_count++] = hash;
}

static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, subject *subj) {
    markdown_core_bufsize initial_pos, after_link_text_pos;
    markdown_core_bufsize endurl, starttitle, endtitle, endall;
    markdown_core_bufsize sps, n;
    markdown_core_reference *ref = NULL;
    markdown_core_chunk url_chunk, title_chunk;
    /* Empty on the reference path, which reaches `match` without writing
     * either: a reference carries no destination. Only the inline `[a](/u)`
     * form fills them, and only it hands them to the node. */
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_chunk title = MARKDOWN_CORE_CHUNK_EMPTY;
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

    /* WHICH definition table answers, decided once.
     *
     * `[^` selects the footnote table, and only when footnotes are enabled:
     * with the option off, `^x` is an ordinary label that `[^x]: /url`
     * defines, and both authorities agree — cmark-gfm and remark each parse
     * `[^x]` + `[^x]: note` as a link reference when footnotes are off and
     * as a footnote when they are on.
     *
     * What `[^` does NOT decide is whether a definition is consulted at all.
     * Both forms exist exactly where their definition does, and both degrade
     * to the bytes the author typed where it does not. That symmetry used to
     * be invisible in the code: the footnote form was reached only by falling
     * out of a failed refmap lookup, so `[^x]` probed the reference table
     * first, always missed, and arrived at its own table through a label
     * named for the miss rather than for the form. */
    if (found_label && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && form == MARKDOWN_CORE_SHORTCUT_REFERENCE &&
        raw_label.len > 1 && raw_label.data[0] == '^') {
        markdown_core_chunk_free(subj->mem, &raw_label);
        goto footnoteForm;
    }

    if (found_label) {
        uint64_t probe_hash = 0;
        ref = (markdown_core_reference *)markdown_core_map_lookup_probe(subj->refmap, &raw_label, &probe_hash);
        S_record_probe(parser, probe_hash);
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
        /* The definition's destination is deliberately not copied here. It
         * was, while a resolved reference was a Link carrying a payload; the
         * clones then outlived their last reader by exactly nothing, because
         * the reference node stores only the label. Copying it back would
         * reintroduce both the leak and the dependency that made every
         * `[foo]: /new` edit reparse each unit mentioning `[foo]`. */
        goto match;
    } else {
        goto noMatch;
    }

noMatch:
footnoteForm:
    /* Two entries, and they mean different things. `footnoteForm` is the
     * decision above arriving deliberately; `noMatch` is a bracket that
     * matched no reference at all and still has to spell its `]`. The
     * footnote block below runs only for the first, because only a `[^`
     * whose interior is one TEXT node reaches its test. */
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
            // The map is the document's, spanning all of it: the lookup sees
            // a definition a hundred lines further down as readily as one
            // above.
            markdown_core_chunk probe = {literal->data + 1, label_span, 0};
            uint64_t probe_hash = 0;
            bool defined = markdown_core_map_lookup_probe(parser->footnote_defs, &probe, &probe_hash) != NULL;
            S_record_probe(parser, probe_hash);

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

            /* The bracket's whole span was reinterpreted as one atomic
             * label, so every record from the opener's candidate on claims
             * consumption the tree no longer shows — an entity or escape
             * inside the label reads verbatim again. A defined reference
             * then owns `[^` and `]` as markup around the preserved label;
             * an undefined one is literal text spelling every byte, and
             * records nothing. */
            markdown_core_concrete_capture_tombstone_from(&subj->capture, opener->concrete_floor);
            if (defined) {
                capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN, opener->position - 1, 2, 2);
                capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, initial_pos - 1, 1, 1);
            }

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

    /* The opener matched: its candidate record becomes markup, the `]`
     * gets its record, and the consumed tail — `(dest "title")`, `[label]`,
     * or `[]` — gets one span. A shortcut reference rewound to just past
     * the `]` and consumes no tail, so it records none. The interior
     * records stay: the link keeps its children. */
    markdown_core_concrete_capture_consume_all(&subj->capture, opener->concrete_floor);
    capture_token(subj, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, initial_pos - 1, 1, 1);
    if (subj->pos > initial_pos) {
        capture_token(
            subj,
            MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL,
            initial_pos,
            subj->pos - initial_pos,
            subj->pos - initial_pos
        );
    }

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
 * the winning binding can consume, allocate, or mutate the delimiter chain.
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
void markdown_core_parse_inlines(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options
) {
    subject subj;
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    const markdown_core_node *refining = parser->refining;
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

    /* Diagnostics raised in here are this unit's, so a refine that is
     * undone takes them with it. */
    parser->refining = parent;
    while (!is_eof(&subj) && parse_inline(parser, &subj, parent, options))
        ;
    parser->refining = refining;

    process_delimiters(parser, &subj, (markdown_core_delimiter_mark){0, 0, 0});
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
    /* Handoff: the parsed node owns its inline records from here — through
     * adoption, the dependent-domain swap, and detach. A failed parse is
     * discarded whole, records included, so a transient loss can never
     * publish a quietly thinner tree. */
    if (subject_has_failure(parser, &subj)) {
        markdown_core_concrete_capture_abandon(&subj.capture);
    } else {
        /* Each inline-owning node is parsed exactly once per refine — a
         * flip or a retract that refines it again frees its records first —
         * so nothing is ever overwritten here. */
        parent->inline_concrete = markdown_core_concrete_capture_take(&subj.capture);
    }
    /* What this unit asked the definition tables becomes its own; the
     * scratch is empty again for the next unit. A unit re-refined after a
     * definition arrived asks afresh, and its old probes go. */
    markdown_core_probes_free(parent->probes);
    parent->probes = NULL;
    if (parser && parser->probe_count) {
        if (!parser->probe_index) {
            parser->probe_index = markdown_core_probe_index_new(parser->mem);
        }
        if (parser->probe_index) {
            parent->probes =
                markdown_core_probes_attach(parser->probe_index, parent, parser->probe_hashes, parser->probe_count);
        }
        if (!parent->probes) {
            parser->oom = true;
        }
        parser->probe_count = 0;
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
    markdown_core_map *refmap,
    markdown_core_reference_spans *spans
) {
    subject subj;

    markdown_core_chunk lab;
    markdown_core_chunk url;
    markdown_core_chunk title;

    markdown_core_bufsize matchlen = 0;
    markdown_core_bufsize beforetitle;

    spans->label_end = 0;
    spans->url_start = 0;
    spans->url_end = 0;
    spans->title_start = 0;
    spans->title_end = 0;

    subject_from_buf(NULL, mem, -1, 0, &subj, input, NULL);

    // parse label:
    if (!link_label(&subj, &lab) || lab.len == 0) {
        return 0;
    }

    // colon:
    if (peek_char(&subj) == ':') {
        advance(&subj);
        spans->label_end = subj.pos;
    } else {
        return 0;
    }

    // parse link url:
    spnl(&subj);
    if ((matchlen = manual_scan_link_url(&subj.input, subj.pos, &url)) > -1) {
        spans->url_start = subj.pos;
        spans->url_end = subj.pos + matchlen;
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
        spans->title_start = subj.pos;
        spans->title_end = subj.pos + matchlen;
        subj.pos += matchlen;
    } else {
        subj.pos = beforetitle;
        title = markdown_core_chunk_literal("");
    }

    // parse final spaces and newline:
    skip_spaces(&subj);
    if (!skip_line_end(&subj)) {
        if (matchlen) { // try rewinding before title
            /* The rewound bytes return to the paragraph, so the definition
             * has no title at all — scalar, map entry, and spans agree
             * (CommonMark: "This is a link reference definition, but it
             * has no title"). Upstream cmark-gfm keeps the scanned title
             * in its map here, a registered deliberate difference. */
            subj.pos = beforetitle;
            title = markdown_core_chunk_literal("");
            spans->title_start = 0;
            spans->title_end = 0;
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

void markdown_core_inline_parser_concrete_use_endpoints(
    markdown_core_inline_parser *parser,
    const markdown_core_delimiter_match *match
) {
    /* Reducers only run inside a real parse, whose subject always engages
     * its capture, and every engine push under an engaged capture records
     * a candidate — so the handles are never zero here. */
    markdown_core_concrete_capture_consume_all(&parser->capture, match->opener_concrete - 1);
    markdown_core_concrete_capture_consume_all(&parser->capture, match->closer_concrete - 1);
}

void markdown_core_inline_parser_concrete_reinterpret(
    markdown_core_inline_parser *parser,
    markdown_core_bufsize start,
    markdown_core_bufsize end
) {
    if (!markdown_core_concrete_capture_retract_span(&parser->capture, (uint32_t)start, (uint32_t)end)) {
        parser->oom = 1;
    }
}

void markdown_core_inline_parser_concrete_capture_spelling(
    markdown_core_inline_parser *parser,
    uint8_t kind,
    markdown_core_bufsize start,
    markdown_core_bufsize end
) {
    capture_token(parser, kind, start, end - start, end - start);
}
