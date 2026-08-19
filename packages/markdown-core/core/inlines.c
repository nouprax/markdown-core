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
#define make_str(scan, sc, ec, s) make_literal(scan, MARKDOWN_CORE_NODE_TEXT, sc, ec, s)
#define make_code(scan, sc, ec, s) make_literal(scan, MARKDOWN_CORE_NODE_CODE, sc, ec, s)
#define make_raw_html(scan, sc, ec, s) make_literal(scan, MARKDOWN_CORE_NODE_HTML, sc, ec, s)
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

struct markdown_core_inline_parser {
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
    /* THE INLINE FRONTIER. A unit's inline stream has a settled prefix and an
     * open tail, exactly as a document has settled blocks and an open spine.
     *
     * `open_from` is the earliest byte whose meaning a byte that has not
     * arrived could still change. A scan that ran to the END OF THE BUFFER
     * without deciding decides differently once the buffer grows, so nothing
     * from where that scan began is settled; a scan that decided on bytes it
     * could all see is decided for good, because a stream only appends. Every
     * handler that can run off the end says so through `inline_parser_note_open`,
     * and one that cannot prove itself confined says so unconditionally — a
     * settled prefix one line short costs a line, one line long is a wrong
     * tree.
     *
     * `settle_at` is the highest position the scan reached where its whole
     * state is the state it started in: a line start, no bracket and no
     * delimiter still waiting for a closer, nothing open behind it, and at
     * least one byte after it. A scan may begin there over these bytes and
     * build exactly what a scan from zero built. */
    markdown_core_bufsize open_from;
    markdown_core_bufsize settle_at;
    /* HOW FAR THE INLINE HANDLER NOW RUNNING SAID IT READ — the byte after
     * the last one it examined, or 0 while it has said nothing. The engine
     * cannot see an extension's own scanners, so silence is read as "to the
     * end of the buffer", which is what every handler was assumed to do
     * before any of them could speak. */
    markdown_core_bufsize read_end;
    /* Sticky allocation-failure flag, copied to the parser after the inline
     * pass so a lossy parse is reported instead of silently truncated. */
    int oom;
    int internal_error;
};

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

static int parse_inline(
    markdown_core_parser *parser,
    markdown_core_inline_parser *scan,
    markdown_core_node *parent,
    int options
);
static int inline_parser_has_failure(const markdown_core_parser *parser, const markdown_core_inline_parser *scan);

static void inline_parser_from_buf(
    markdown_core_parser *parser,
    markdown_core_mem *mem,
    int line_number,
    int block_offset,
    markdown_core_inline_parser *e,
    markdown_core_chunk *buffer,
    markdown_core_map *refmap
);
static markdown_core_bufsize inline_parser_find_special_char(markdown_core_inline_parser *scan, int options);
static void inline_parser_set_delimiter_failure(
    markdown_core_inline_parser *scan,
    markdown_core_delimiter_result result
);

// Create an inline with a literal string value.
static MARKDOWN_CORE_INLINE markdown_core_node *make_literal(
    markdown_core_inline_parser *scan,
    markdown_core_node_type t,
    int start_column,
    int end_column,
    markdown_core_chunk s
) {
    markdown_core_node *e = (markdown_core_node *)scan->mem->calloc(scan->mem, 1, sizeof(*e));
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(scan->mem, &s);
        scan->oom = 1;
        return NULL;
    }
    markdown_core_strbuf_init(scan->mem, &e->content, 0);
    e->type = (uint16_t)t;
    e->as.literal = s;
    e->start_line = e->end_line = scan->line;
    // columns are 1 based.
    e->start_column = start_column + 1 + scan->column_offset + scan->block_offset;
    e->end_column = end_column + 1 + scan->column_offset + scan->block_offset;
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

/* make_simple with the scan's loss flag for handlers that consume input
 * before creating the node. */
static MARKDOWN_CORE_INLINE markdown_core_node *make_simple_subj(
    markdown_core_inline_parser *scan,
    markdown_core_node_type t
) {
    markdown_core_node *e = make_simple(scan->mem, t);
    if (!e) {
        scan->oom = 1;
    }
    return e;
}

// Like make_str, but parses entities.
static markdown_core_node *make_str_with_entities(
    markdown_core_inline_parser *scan,
    int start_column,
    int end_column,
    markdown_core_chunk *content
) {
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT(scan->mem);

    if (markdown_core_houdini_unescape_html(&unescaped, content->data, content->len)) {
        if (unescaped.oom) {
            scan->oom = 1;
        }
        return make_str(scan, start_column, end_column, markdown_core_chunk_buf_detach(&unescaped));
    } else {
        return make_str(scan, start_column, end_column, *content);
    }
}

static markdown_core_chunk markdown_core_clean_autolink(
    markdown_core_inline_parser *scan,
    markdown_core_chunk *url,
    int is_email
) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(scan->mem);

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
        scan->oom = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

static MARKDOWN_CORE_INLINE markdown_core_node *make_autolink(
    markdown_core_inline_parser *scan,
    int start_column,
    int end_column,
    markdown_core_chunk url,
    int is_email
) {
    markdown_core_node *link = make_simple(scan->mem, MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *text;
    if (!link) {
        scan->oom = 1;
        return NULL;
    }
    link->as.link.url = markdown_core_clean_autolink(scan, &url, is_email);
    /* No title, and the node is calloc'd, so the field is already the empty
     * chunk. Left unset deliberately: an autolink has nowhere to write a
     * title, and a NULL chunk is how the tree says "not written" — the same
     * distinction `[t](/u "")` and `[t](/u)` already carry. Assigning the ""
     * literal here reported a title the author never wrote. */
    link->start_line = link->end_line = scan->line;
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
    link->start_column = start_column + 1 + scan->column_offset + scan->block_offset;
    link->end_column = end_column + 1 + scan->column_offset + scan->block_offset;
    text = make_str_with_entities(scan, start_column + 1, end_column - 1, &url);
    if (text) {
        markdown_core_node_append_child_unchecked(link, text);
    }
    return link;
}

static void inline_parser_from_buf(
    markdown_core_parser *parser,
    markdown_core_mem *mem,
    int line_number,
    int block_offset,
    markdown_core_inline_parser *e,
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
        inline_parser_set_delimiter_failure(
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
    /* Nothing open and nothing settled until the scan says otherwise. The
     * buffer's own length is the "nothing open" value: every position in it
     * is at or before the end, so the settle test reads the same whether a
     * handler has spoken or not. */
    e->open_from = chunk->len;
    e->settle_at = 0;
    e->read_end = 0;
}

/* A handler that read to the end of the buffer without deciding: everything
 * from `start` on is a later scan's to derive again. Monotone downward — the
 * earliest such start is the one that bounds the settled prefix. */
static MARKDOWN_CORE_INLINE void inline_parser_note_open(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize start
) {
    if (start < scan->open_from) {
        scan->open_from = start;
    }
}

static MARKDOWN_CORE_INLINE int isbacktick(int c) { return (c == '`'); }

static MARKDOWN_CORE_INLINE unsigned char peek_char_n(markdown_core_inline_parser *scan, markdown_core_bufsize n) {
    // NULL bytes should have been stripped out by now.  If they're
    // present, it's a programming error:
    assert(!(scan->pos + n < scan->input.len && scan->input.data[scan->pos + n] == 0));
    return (scan->pos + n < scan->input.len) ? scan->input.data[scan->pos + n] : 0;
}

static MARKDOWN_CORE_INLINE unsigned char peek_char(markdown_core_inline_parser *scan) { return peek_char_n(scan, 0); }

static MARKDOWN_CORE_INLINE unsigned char peek_at(markdown_core_inline_parser *scan, markdown_core_bufsize pos) {
    return scan->input.data[pos];
}

// Return true if there are more characters in the markdown_core_inline_parser.
static MARKDOWN_CORE_INLINE int is_eof(markdown_core_inline_parser *scan) { return (scan->pos >= scan->input.len); }

// Advance the markdown_core_inline_parser.  Doesn't check for eof.
#define advance(scan) (scan)->pos += 1

static MARKDOWN_CORE_INLINE bool skip_spaces(markdown_core_inline_parser *scan) {
    bool skipped = false;
    while (peek_char(scan) == ' ' || peek_char(scan) == '\t') {
        advance(scan);
        skipped = true;
    }
    return skipped;
}

static MARKDOWN_CORE_INLINE bool skip_line_end(markdown_core_inline_parser *scan) {
    bool seen_line_end_char = false;
    if (peek_char(scan) == '\r') {
        advance(scan);
        seen_line_end_char = true;
    }
    if (peek_char(scan) == '\n') {
        advance(scan);
        seen_line_end_char = true;
    }
    return seen_line_end_char || is_eof(scan);
}

// Take characters while a predicate holds, and return a string.
static MARKDOWN_CORE_INLINE markdown_core_chunk take_while(markdown_core_inline_parser *scan, int (*f)(int)) {
    unsigned char c;
    markdown_core_bufsize startpos = scan->pos;
    markdown_core_bufsize len = 0;

    while ((c = peek_char(scan)) && (*f)(c)) {
        advance(scan);
        len++;
    }

    return markdown_core_chunk_borrow(&scan->input, startpos, len);
}

// Return the number of newlines in a given span of text in a markdown_core_inline_parser.  If
// the number is greater than zero, also return the number of characters
// between the last newline and the end of the span in `since_newline`.
static int count_newlines(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize from,
    markdown_core_bufsize len,
    int *since_newline
) {
    int nls = 0;
    int since_nl = 0;

    while (len--) {
        if (scan->input.data[from++] == '\n') {
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

static void inline_parser_set_delimiter_failure(
    markdown_core_inline_parser *scan,
    markdown_core_delimiter_result result
) {
    if (result == MARKDOWN_CORE_DELIMITER_OOM) {
        scan->oom = 1;
    } else if (result != MARKDOWN_CORE_DELIMITER_OK) {
        scan->internal_error = 1;
    }
}

/* Appends one inline concrete record for the token the calling handler just
 * consumed. Every caller runs under parse_inline, so the capture is always
 * engaged; a lost record joins the scan's sticky failure and the parse
 * is discarded rather than published thinner (the OOM sweep's property). */
static void capture_token(
    markdown_core_inline_parser *scan,
    uint8_t kind,
    markdown_core_bufsize start,
    markdown_core_bufsize length,
    markdown_core_bufsize consumed
) {
    if (!markdown_core_concrete_capture_append(
            &scan->capture,
            kind,
            (uint32_t)start,
            (uint32_t)length,
            (uint32_t)consumed,
            0
        )) {
        scan->oom = 1;
    }
}

static uint64_t inline_parser_next_claim_order(markdown_core_inline_parser *scan) {
    if (scan->claim_clock == UINT64_MAX) {
        scan->internal_error = 1;
        return 0;
    }
    return scan->claim_clock + 1;
}

static int source_span_through(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize end,
    markdown_core_inline_source_span *span
) {
    int newlines;
    int since_newline = 0;

    if (!scan || !span || scan->phase != INLINE_PHASE_SCAN || end <= scan->pos || end > scan->input.len) {
        if (scan) {
            scan->internal_error = 1;
        }
        return 0;
    }

    span->start_line = scan->line;
    span->start_column = scan->pos + 1 + scan->column_offset + scan->block_offset;
    newlines = count_newlines(scan, scan->pos, end - scan->pos, &since_newline);
    span->end_line = scan->line + newlines;
    span->end_column = newlines ? since_newline : span->start_column + (int)(end - scan->pos) - 1;
    return 1;
}

static void commit_source_span(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize end,
    const markdown_core_inline_source_span *span
) {
    if (span->end_line != span->start_line) {
        scan->line = span->end_line;
        scan->column_offset = -end + span->end_column;
    }
    scan->pos = end;
}

static markdown_core_node *stage_source_text(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize start,
    markdown_core_bufsize end,
    const markdown_core_inline_source_span *span
) {
    markdown_core_node *node = make_simple_subj(scan, MARKDOWN_CORE_NODE_TEXT);
    if (!node) {
        return NULL;
    }
    node->as.literal = markdown_core_chunk_borrow(&scan->input, start, end - start);
    node->start_line = span->start_line;
    node->start_column = span->start_column;
    node->end_line = span->end_line;
    node->end_column = span->end_column;
    return node;
}

// Adjust `node`'s `end_line`, `end_column`, and `scan`'s `line` and
// `column_offset` according to the number of newlines in a just-matched span
// of text in `scan`.  Scope tracking is mandatory (canonical-ast.md), so this
// always runs; it was a render-era option in cmark.
static void adjust_subj_node_newlines(
    markdown_core_inline_parser *scan,
    markdown_core_node *node,
    int matchlen,
    int extra
) {
    int since_newline;
    int newlines = count_newlines(scan, scan->pos - matchlen - extra, matchlen, &since_newline);
    if (newlines) {
        scan->line += newlines;
        node->end_line += newlines;
        node->end_column = since_newline;
        scan->column_offset = -scan->pos + since_newline + extra;
    }
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the markdown_core_inline_parser
// after the closing backticks.
static markdown_core_bufsize scan_to_closing_backticks(
    markdown_core_inline_parser *scan,
    markdown_core_bufsize openticklength
) {

    bool found = false;
    if (openticklength > MAXBACKTICKS) {
        // we limit backtick string length because of the array scan->backticks:
        return 0;
    }
    if (scan->scanned_for_backticks && scan->backticks[openticklength] <= scan->pos) {
        // return if we already know there's no closer
        return 0;
    }
    while (!found) {
        // read non backticks
        unsigned char c;
        while ((c = peek_char(scan)) && c != '`') {
            advance(scan);
        }
        if (is_eof(scan)) {
            break;
        }
        markdown_core_bufsize numticks = 0;
        while (peek_char(scan) == '`') {
            advance(scan);
            numticks++;
        }
        // store position of ender
        if (numticks <= MAXBACKTICKS) {
            scan->backticks[numticks] = scan->pos - numticks;
        }
        if (numticks == openticklength) {
            return (scan->pos);
        }
    }
    // got through whole input without finding closer
    scan->scanned_for_backticks = true;
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
// Assumes that the markdown_core_inline_parser has a backtick at the current position.
static markdown_core_node *handle_backticks(markdown_core_inline_parser *scan, int options) {
    markdown_core_chunk openticks = take_while(scan, isbacktick);
    markdown_core_bufsize startpos = scan->pos;
    markdown_core_bufsize endpos = scan_to_closing_backticks(scan, openticks.len);

    if (endpos == 0) {        // not found
        scan->pos = startpos; // rewind
        /* scan_to_closing_backticks answers 0 only after reading through the
         * whole buffer (or after an earlier scan did and cached it), so this
         * run is open: a closer arriving later makes it a code span. */
        inline_parser_note_open(scan, startpos - openticks.len);
        return make_str(scan, scan->pos, scan->pos, openticks);
    } else {
        markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(scan->mem);

        markdown_core_strbuf_set(&buf, scan->input.data + startpos, endpos - startpos - openticks.len);
        S_normalize_code(&buf);
        if (buf.oom) {
            scan->oom = 1;
        }

        /* Both tick runs vanish from the projection (the Code literal is
         * the normalized interior), so both are markup material; the
         * matched closer's run length equals the opener's. */
        capture_token(
            scan,
            MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS,
            startpos - openticks.len,
            openticks.len,
            openticks.len
        );
        capture_token(
            scan,
            MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS,
            endpos - openticks.len,
            openticks.len,
            openticks.len
        );

        markdown_core_node *node =
            make_code(scan, startpos, endpos - openticks.len - 1, markdown_core_chunk_buf_detach(&buf));
        if (!node) {
            return NULL;
        }
        adjust_subj_node_newlines(scan, node, endpos - startpos, openticks.len);
        return node;
    }
}

// Scan ***, **, or * and return number scanned, or 0.
// Advances position.
static int scan_delims(markdown_core_inline_parser *scan, unsigned char c, bool *can_open, bool *can_close) {
    int numdelims = 0;
    markdown_core_bufsize before_char_pos, after_char_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool left_flanking, right_flanking;

    if (scan->pos == 0) {
        before_char = 10;
    } else {
        before_char_pos = scan->pos - 1;
        // walk back to the beginning of the UTF_8 sequence:
        while ((peek_at(scan, before_char_pos) >> 6 == 2 || scan->skip_chars[peek_at(scan, before_char_pos)]) &&
               before_char_pos > 0) {
            before_char_pos -= 1;
        }
        len = markdown_core_utf8proc_iterate(
            scan->input.data + before_char_pos,
            scan->pos - before_char_pos,
            &before_char
        );
        if (len == -1 || (before_char < 256 && scan->skip_chars[(unsigned char)before_char])) {
            before_char = 10;
        }
    }

    if (c == '\'' || c == '"') {
        numdelims++;
        advance(scan); // limit to 1 delim for quotes
    } else {
        while (peek_char(scan) == c) {
            numdelims++;
            advance(scan);
        }
    }

    if (scan->pos == scan->input.len) {
        after_char = 10;
    } else {
        after_char_pos = scan->pos;
        while (after_char_pos < scan->input.len && scan->skip_chars[peek_at(scan, after_char_pos)]) {
            after_char_pos += 1;
        }
        len = markdown_core_utf8proc_iterate(
            scan->input.data + after_char_pos,
            scan->input.len - after_char_pos,
            &after_char
        );
        if (len == -1 || (after_char < 256 && scan->skip_chars[(unsigned char)after_char])) {
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

static void pop_bracket(markdown_core_inline_parser *scan) {
    bracket *b;
    if (scan->last_bracket == NULL) {
        return;
    }
    b = scan->last_bracket;
    scan->last_bracket = scan->last_bracket->previous;
    scan->mem->free(scan->mem, b);
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

static void push_bracket(markdown_core_inline_parser *scan, bool image, markdown_core_node *inl_text) {
    bracket *b = (bracket *)scan->mem->calloc(scan->mem, 1, sizeof(bracket));
    uint64_t claim_order;
    if (!b) {
        scan->oom = 1;
        return;
    }
    claim_order = inline_parser_next_claim_order(scan);
    if (!claim_order) {
        scan->mem->free(scan->mem, b);
        return;
    }
    /* The opener's candidate record: consumed only if this bracket
     * matches, retracted wholesale if a footnote reference swallows it.
     * Its index doubles as both the patch key and the retraction floor. */
    b->concrete_floor = markdown_core_concrete_capture_count(&scan->capture);
    capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, scan->pos - (image ? 2 : 1), image ? 2 : 1, 0);
    if (scan->oom) {
        scan->mem->free(scan->mem, b);
        return;
    }
    if (scan->last_bracket != NULL) {
        scan->last_bracket->bracket_after = true;
        b->in_bracket_image0 = scan->last_bracket->in_bracket_image0;
        b->in_bracket_image1 = scan->last_bracket->in_bracket_image1;
    }
    b->image = image;
    b->active = true;
    b->inl_text = inl_text;
    b->previous = scan->last_bracket;
    b->position = scan->pos;
    b->claim_order = claim_order;
    b->delimiter_mark = markdown_core_delimiter_engine_mark(scan->delimiters);
    b->bracket_after = false;
    if (image) {
        b->in_bracket_image1 = true;
    } else {
        b->in_bracket_image0 = true;
    }
    scan->last_bracket = b;
    scan->claim_clock = claim_order;
    if (!image) {
        scan->no_link_openers = false;
    }
}

// Assumes the markdown_core_inline_parser has a c at the current position.
static markdown_core_node *handle_delim(markdown_core_inline_parser *scan, unsigned char c, bool smart) {
    markdown_core_bufsize numdelims;
    markdown_core_node *inl_text;
    bool can_open, can_close;
    markdown_core_chunk contents;

    numdelims = scan_delims(scan, c, &can_open, &can_close);

    if (c == '\'' && smart) {
        contents = markdown_core_chunk_literal(RIGHTSINGLEQUOTE);
    } else if (c == '"' && smart) {
        contents = markdown_core_chunk_literal(can_close ? RIGHTDOUBLEQUOTE : LEFTDOUBLEQUOTE);
    } else {
        contents = markdown_core_chunk_borrow(&scan->input, scan->pos - numdelims, numdelims);
    }

    inl_text = make_str(scan, scan->pos - numdelims, scan->pos - 1, contents);

    if (inl_text && (can_open || can_close) && (!(c == '\'' || c == '"') || smart)) {
        markdown_core_delimiter_result result = markdown_core_delimiter_engine_push(
            scan->delimiters,
            core_delimiter_binding(c),
            can_open,
            can_close,
            inl_text,
            scan->pos - numdelims,
            scan->pos,
            0
        );
        inline_parser_set_delimiter_failure(scan, result);
        /* A smart quote's source byte was already replaced by its curly
         * glyph above, so the token is fully consumed whether or not it
         * later pairs; rewrite the push's generic run record to say so. */
        if (result == MARKDOWN_CORE_DELIMITER_OK && smart && (c == '\'' || c == '"')) {
            size_t index = markdown_core_concrete_capture_count(&scan->capture) - 1;
            markdown_core_concrete_capture_set_kind(&scan->capture, index, MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE);
            markdown_core_concrete_capture_consume_all(&scan->capture, index);
        }
    } else if (inl_text && smart && (c == '\'' || c == '"')) {
        /* Replaced but not flanking: no delimiter push happens, yet the
         * spelling is gone all the same. */
        capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, scan->pos - numdelims, numdelims, numdelims);
    }

    return inl_text;
}

// Assumes we have a hyphen at the current position.
static markdown_core_node *handle_hyphen(markdown_core_inline_parser *scan, bool smart) {
    int startpos = scan->pos;

    advance(scan);

    if (!smart || peek_char(scan) != '-') {
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("-"));
    }

    while (smart && peek_char(scan) == '-') {
        advance(scan);
    }

    int numhyphens = scan->pos - startpos;
    int en_count = 0;
    int em_count = 0;
    int i;
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(scan->mem);

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
        scan->oom = 1;
    }
    /* A run of two or more was rewritten into dashes; a lone hyphen keeps
     * its own byte and records nothing (the early return above). */
    capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH, startpos, numhyphens, numhyphens);
    return make_str(scan, startpos, scan->pos - 1, markdown_core_chunk_buf_detach(&buf));
}

// Assumes we have a period at the current position.
static markdown_core_node *handle_period(markdown_core_inline_parser *scan, bool smart) {
    advance(scan);
    if (smart && peek_char(scan) == '.') {
        advance(scan);
        if (peek_char(scan) == '.') {
            advance(scan);
            /* Exactly `...` became an ellipsis; `..` and `.` keep their
             * bytes below and record nothing. */
            capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS, scan->pos - 3, 3, 3);
            return make_str(scan, scan->pos - 3, scan->pos - 1, markdown_core_chunk_literal(ELLIPSES));
        } else {
            return make_str(scan, scan->pos - 2, scan->pos - 1, markdown_core_chunk_literal(".."));
        }
    } else {
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("."));
    }
}

/* THE INLINE STREAM SETTLES AS IT IS SCANNED, the way the block stream does.
 * At a line start the scan pairs everything on the delimiter stack that can
 * be paired and leaves standing what cannot — a `*` whose closer has not been
 * reached yet keeps its place, and a pair a line closed becomes the Emphasis
 * it was always going to become.
 *
 * The result is the result: a closer's opener is the nearest eligible one
 * before it, and no byte scanned later stands before it, so pairing early
 * pairs the same way. What it buys is a stack that EMPTIES. Left to the one
 * pass at the end, the stack only grows, and an empty stack is what says that
 * nothing behind this point can still be restructured — the fact an
 * incremental refine of an open leaf has to stand on.
 *
 * Never under an open bracket: that region is not closed, and reducing into
 * it would let a closer between the brackets match an opener outside them.
 * A line start is the granularity because it is the coarsest point that costs
 * nothing to find and the finest that a unit's own frontier can use. */
static void inline_parser_settle_delimiters(markdown_core_parser *parser, markdown_core_inline_parser *scan) {
    if (!scan->delimiters || scan->last_bracket || scan->pos == 0 || scan->pos >= scan->input.len) {
        return;
    }
    if (scan->input.data[scan->pos - 1] != '\n' || scan->phase != INLINE_PHASE_SCAN) {
        return;
    }
    scan->phase = INLINE_PHASE_REDUCE;
    inline_parser_set_delimiter_failure(scan, markdown_core_delimiter_engine_settle(scan->delimiters, parser, scan));
    scan->phase = INLINE_PHASE_SCAN;
    /* AND THIS IS THE FRONTIER. The settle left the stack empty, no bracket
     * is open, the line ending behind is one no later byte can reinterpret
     * (the input is right-trimmed, and a trailing run of whitespace can only
     * move away from here), and nothing behind read past the buffer's end. So
     * the scan stands exactly where it stood when it began, and everything
     * before this point is what any longer buffer would build too. */
    if (scan->delimiters->head || scan->open_from < scan->pos) {
        return;
    }
    scan->settle_at = scan->pos;
}

static void process_delimiters(
    markdown_core_parser *parser,
    markdown_core_inline_parser *scan,
    markdown_core_delimiter_mark mark
) {
    markdown_core_delimiter_result result;
    if (scan->phase != INLINE_PHASE_SCAN) {
        scan->internal_error = 1;
        return;
    }
    scan->phase = INLINE_PHASE_REDUCE;
    result = inline_parser_has_failure(parser, scan)
                 ? markdown_core_delimiter_engine_truncate(scan->delimiters, mark)
                 : markdown_core_delimiter_engine_process(scan->delimiters, parser, scan, mark);
    scan->phase = INLINE_PHASE_SCAN;
    inline_parser_set_delimiter_failure(scan, result);
}

static markdown_core_delimiter_result S_reduce_emph(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    markdown_core_inline_parser *scan = inline_parser;
    markdown_core_node *opener_inl = match->opener_node;
    markdown_core_node *closer_inl = match->closer_node;
    markdown_core_bufsize opener_num_chars = match->opener_remaining - match->use_length;
    markdown_core_bufsize closer_num_chars = match->closer_remaining - match->use_length;
    markdown_core_node *tmp, *tmpnext, *emph;

    /* Allocate before changing either endpoint. An OOM parse is discarded,
     * but the local AST still remains internally consistent for cleanup. */
    emph = match->use_length == 1 ? make_emphasis(scan->mem) : make_strong(scan->mem);
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
    markdown_core_inline_parser *scan = inline_parser;
    const char *left = match->kind == CORE_DELIMITER_SINGLE_QUOTE ? LEFTSINGLEQUOTE : LEFTDOUBLEQUOTE;
    const char *right = match->kind == CORE_DELIMITER_SINGLE_QUOTE ? RIGHTSINGLEQUOTE : RIGHTDOUBLEQUOTE;
    markdown_core_chunk_free(scan->mem, &match->opener_node->as.literal);
    markdown_core_chunk_free(scan->mem, &match->closer_node->as.literal);
    match->opener_node->as.literal = markdown_core_chunk_literal(left);
    match->closer_node->as.literal = markdown_core_chunk_literal(right);
    return MARKDOWN_CORE_DELIMITER_OK;
}

// Parse backslash-escape or just a backslash, returning an inline.
static markdown_core_node *handle_backslash(markdown_core_parser *parser, markdown_core_inline_parser *scan) {
    markdown_core_bufsize start = scan->pos;
    advance(scan);
    unsigned char nextchar = peek_char(scan);
    if (markdown_core_ispunct(nextchar)) {
        if (nextchar == '\\' && parser->inline_config->dispatch['\\'].count == 0) {
            markdown_core_bufsize end = start;
            markdown_core_bufsize pair;
            while (end + 1 < scan->input.len && scan->input.data[end] == '\\' && scan->input.data[end + 1] == '\\') {
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
                capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, pair, 1, 1);
            }
            scan->pos = end;
            return make_str(scan, start, end - 1, markdown_core_chunk_borrow(&scan->input, start, (end - start) / 2));
        }
        // only ascii symbols and newline can be escaped
        advance(scan);
        capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, start, 1, 1);
        return make_str(scan, scan->pos - 2, scan->pos - 1, markdown_core_chunk_borrow(&scan->input, scan->pos - 1, 1));
    } else if (!is_eof(scan) && skip_line_end(scan)) {
        capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, start, 1, 1);
        return make_simple_subj(scan, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("\\"));
    }
}

// Parse an entity or a regular "&" string.
// Assumes the markdown_core_inline_parser has an '&' character at the current position.
static markdown_core_node *handle_entity(markdown_core_inline_parser *scan) {
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT(scan->mem);
    markdown_core_bufsize len;

    advance(scan);

    len = markdown_core_houdini_unescape_ent(&ent, scan->input.data + scan->pos, scan->input.len - scan->pos);

    if (len == 0) {
        /* The entity scan reads a bounded window, so it is open only when
         * that window reached the buffer's end — an `&` with a whole window
         * of settled bytes behind its failure is an ampersand for good. */
        if (scan->pos + markdown_core_houdini_entity_window() > scan->input.len) {
            inline_parser_note_open(scan, scan->pos - 1);
        }
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("&"));
    }

    scan->pos += len;
    if (ent.oom) {
        scan->oom = 1;
    }
    /* The full spelling, `&` through `;`, decoded away into the text. */
    capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_ENTITY, scan->pos - 1 - len, len + 1, len + 1);
    return make_str(scan, scan->pos - 1 - len, scan->pos - 1, markdown_core_chunk_buf_detach(&ent));
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
// Assumes the markdown_core_inline_parser has a '<' character at the current position.
static markdown_core_node *handle_pointy_brace(markdown_core_inline_parser *scan, int options) {
    markdown_core_bufsize matchlen = 0;
    markdown_core_chunk contents;

    advance(scan); // advance past first <

    // first try to match a URL autolink
    matchlen = scan_autolink_uri(&scan->input, scan->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&scan->input, scan->pos, matchlen - 1);
        scan->pos += matchlen;

        /* The whole `<...>` construct: brackets consumed, interior decoded
         * into the link's url and text. Raw HTML below stays recordless —
         * its literal is the exact source bytes. */
        capture_token(
            scan,
            MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK,
            scan->pos - 1 - matchlen,
            matchlen + 1,
            matchlen + 1
        );
        return make_autolink(scan, scan->pos - 1 - matchlen, scan->pos - 1, contents, 0);
    }

    // next try to match an email autolink
    matchlen = scan_autolink_email(&scan->input, scan->pos);
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&scan->input, scan->pos, matchlen - 1);
        scan->pos += matchlen;

        capture_token(
            scan,
            MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK,
            scan->pos - 1 - matchlen,
            matchlen + 1,
            matchlen + 1
        );
        return make_autolink(scan, scan->pos - 1 - matchlen, scan->pos - 1, contents, 1);
    }

    // finally, try to match an html tag
    if (scan->pos + 2 <= scan->input.len) {
        int c = scan->input.data[scan->pos];
        if (c == '!' && (scan->flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = scan->input.data[scan->pos + 1];
            if (c == '-' && scan->input.data[scan->pos + 2] == '-') {
                if (scan->input.data[scan->pos + 3] == '>') {
                    matchlen = 4;
                } else if (scan->input.data[scan->pos + 3] == '-' && scan->input.data[scan->pos + 4] == '>') {
                    matchlen = 5;
                } else {
                    matchlen = scan_html_comment(&scan->input, scan->pos + 1);
                    if (matchlen > 0) {
                        matchlen += 1; // prefix "<"
                    } else {           // no match through end of input: set a flag so
                                       // we don't reparse looking for -->:
                        scan->flags |= FLAG_SKIP_HTML_COMMENT;
                    }
                }
            } else if (c == '[') {
                if ((scan->flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(&scan->input, scan->pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (scan->pos + matchlen > scan->input.len) {
                            scan->flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((scan->flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(&scan->input, scan->pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (scan->pos + matchlen > scan->input.len) {
                        scan->flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((scan->flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(&scan->input, scan->pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (scan->pos + matchlen > scan->input.len) {
                    scan->flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(&scan->input, scan->pos);
        }
    }
    if (matchlen > 0) {
        contents = markdown_core_chunk_borrow(&scan->input, scan->pos - 1, matchlen + 1);
        scan->pos += matchlen;
        /* Raw HTML keeps its exact source bytes as its literal and records
         * nothing — comments included: a comment is an ordinary HTML node
         * the consumer classifies through the facade's comment bit, never
         * a deletion. */
        markdown_core_node *node = make_raw_html(scan, scan->pos - matchlen - 1, scan->pos - 1, contents);
        if (!node) {
            return NULL;
        }
        adjust_subj_node_newlines(scan, node, matchlen, 1);
        return node;
    }

    // if nothing matches, just return the opening <:
    /* A `<` THAT DID NOT MATCH IS OPEN, always. The first cut of this rule
     * asked whether a `>` the scans could have ended on already stood in the
     * buffer, and read a `>` as proof that the failure was final. It is not:
     * a tag's attribute value is quoted, a quoted value may contain `>` and
     * may span lines, and a scan that ran off the end looking for the
     * closing quote fails now and succeeds when the rest arrives —
     * `<a foo="bar" bam = 'baz <em>"</em>'` followed by a line ending in
     * `/>` is one tag, and the `>` inside `<em>` is not the one that
     * decides it. The scanners do not say how far they read, so a `<` that
     * matched nothing costs its paragraph a settle from here. */
    inline_parser_note_open(scan, scan->pos - 1);
    return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("<"));
}

// Parse a link label.  Returns 1 if successful.
// Note:  unescaped brackets are not allowed in labels.
// The label begins with `[` and ends with the first `]` character
// encountered.  Backticks in labels do not start code spans.
static int link_label(markdown_core_inline_parser *scan, markdown_core_chunk *raw_label) {
    markdown_core_bufsize startpos = scan->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (peek_char(scan) == '[') {
        advance(scan);
    } else {
        return 0;
    }

    while ((c = peek_char(scan)) && c != '[' && c != ']') {
        if (c == '\\') {
            advance(scan);
            length++;
            if (markdown_core_ispunct(peek_char(scan))) {
                advance(scan);
                length++;
            }
        } else {
            advance(scan);
            length++;
        }
        if (length > MAX_LINK_LABEL_LENGTH) {
            goto noMatch;
        }
    }

    if (c == ']') { // match found
        *raw_label = markdown_core_chunk_borrow(&scan->input, startpos + 1, scan->pos - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        advance(scan); // advance past ]
        return 1;
    }

noMatch:
    scan->pos = startpos; // rewind
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
    markdown_core_inline_parser *scan,
    const markdown_core_chunk *literal,
    int label_span,
    bool defined
) {
    markdown_core_node *node;
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(scan->mem);

    if (defined) {
        node = make_simple(scan->mem, MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE);
        if (node) {
            node->as.literal = markdown_core_chunk_borrow(literal, 1, label_span);
        }
        return node;
    }

    node = make_simple(scan->mem, MARKDOWN_CORE_NODE_TEXT);
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

static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, markdown_core_inline_parser *scan) {
    markdown_core_bufsize initial_pos, after_link_text_pos;
    markdown_core_bufsize opener_position = 0;
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

    advance(scan); // advance past ]
    initial_pos = scan->pos;

    // get last [ or ![
    opener = scan->last_bracket;

    if (opener == NULL) {
        /* No opener to complete: a `]` alone is a `]` for good. */
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));
    }
    /* Kept because the failing paths pop the bracket before they can say
     * where it stood, and the frontier is measured from the opener. */
    opener_position = opener->position - 1;

    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    is_image = opener->image;

    if (!is_image && scan->no_link_openers) {
        // take delimiter off stack
        pop_bracket(scan);
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));
    }

    after_link_text_pos = scan->pos;

    // First, look for an inline link.
    if (peek_char(scan) == '(' && ((sps = scan_spacechars(&scan->input, scan->pos + 1)) > -1) &&
        ((n = manual_scan_link_url(&scan->input, scan->pos + 1 + sps, &url_chunk)) > -1)) {

        // try to parse an explicit link:
        endurl = scan->pos + 1 + sps + n;
        starttitle = endurl + scan_spacechars(&scan->input, endurl);

        // ensure there are spaces btw url and title
        endtitle = (starttitle == endurl) ? starttitle : starttitle + scan_link_title(&scan->input, starttitle);

        endall = endtitle + scan_spacechars(&scan->input, endtitle);

        if (peek_at(scan, endall) == ')') {
            scan->pos = endall + 1;

            title_chunk = markdown_core_chunk_borrow(&scan->input, starttitle, endtitle - starttitle);
            {
                int lost = 0;
                url = markdown_core_clean_url(scan->mem, &url_chunk, &lost);
                title = markdown_core_clean_title(scan->mem, &title_chunk, &lost);
                if (lost) {
                    scan->oom = 1;
                }
            }
            markdown_core_chunk_free(scan->mem, &url_chunk);
            markdown_core_chunk_free(scan->mem, &title_chunk);
            goto match;

        } else {
            // it could still be a shortcut reference link
            scan->pos = after_link_text_pos;
        }
    }

    // Next, look for a following [link label] that matches in refmap.
    // skip spaces
    raw_label = markdown_core_chunk_literal("");
    found_label = link_label(scan, &raw_label);
    if (found_label) {
        form = raw_label.len ? MARKDOWN_CORE_FULL_REFERENCE : MARKDOWN_CORE_COLLAPSED_REFERENCE;
    }
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        scan->pos = initial_pos;
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(scan->mem, &raw_label);
        raw_label = markdown_core_chunk_borrow(&scan->input, opener->position, initial_pos - opener->position - 1);
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
        markdown_core_chunk_free(scan->mem, &raw_label);
        goto footnoteForm;
    }

    if (found_label) {
        uint64_t probe_hash = 0;
        ref = (markdown_core_reference *)markdown_core_map_lookup_probe(scan->refmap, &raw_label, &probe_hash);
        S_record_probe(parser, probe_hash);
        if (ref != NULL) {
            /* Kept for the node: the label as written, which is what the
             * source says. Its normalized form stays the map's. chunk_dup
             * borrows the block's content, which the node outlives. */
            label = raw_label;
            if (!markdown_core_chunk_to_cstr(scan->mem, &label)) {
                scan->oom = 1;
            }
            is_reference = true;
        } else {
            markdown_core_chunk_free(scan->mem, &raw_label);
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
                (initial_pos + scan->column_offset + scan->block_offset) - opener->inl_text->start_column - 2;
            bool label_blank = true;
            for (int i = 0; i < label_span && label_blank; i++) {
                label_blank = markdown_core_isspace(literal->data[1 + i]);
            }
            if (label_blank) {
                pop_bracket(scan);
                scan->pos = initial_pos;
                return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));
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
            // Let's just rewind the scan's position:
            scan->pos = initial_pos;

            // the start and end of the footnote ref is the opening and closing brace
            // i.e. the scan's current position, and the opener's start_column
            int fnref_end_column = scan->pos + scan->column_offset + scan->block_offset;
            int fnref_start_column = opener->inl_text->start_column;

            // any given node delineates a substring of the line being processed,
            // with the remainder of the line being pointed to thru its 'literal'
            // struct member.
            // here, we copy the literal's pointer, moving it past the '^' character
            // for a length equal to the size of footnote reference text.
            // i.e. end_col minus start_col, minus the [ and the ^ characters
            //
            // this copies the footnote reference string, even if between the
            // `opener` and the scan's current position there are other nodes
            markdown_core_node *fnref = make_footnote_reference_or_text(scan, literal, label_span, defined);
            if (!fnref) {
                scan->oom = 1;
                pop_bracket(scan);
                return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));
            }

            fnref->start_line = fnref->end_line = scan->line;
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
            markdown_core_concrete_capture_tombstone_from(&scan->capture, opener->concrete_floor);
            if (defined) {
                capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN, opener->position - 1, 2, 2);
                capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, initial_pos - 1, 1, 1);
            }

            process_delimiters(parser, scan, opener->delimiter_mark);
            // sometimes, the footnote reference text gets parsed into multiple nodes
            // i.e. '[^example]' parsed into '[', '^exam', 'ple]'.
            // this happens for ex with the autolink extension. when the autolinker
            // finds the 'w' character, it will split the text into multiple nodes
            // in hopes of being able to match a 'www.' substring.
            //
            // because this function is called one character at a time via the
            // `parse_inlines` function, and the current scan->pos is pointing at the
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

            pop_bracket(scan);
            return NULL;
        }
    }

    pop_bracket(scan); // remove this opener from delimiter list
    scan->pos = initial_pos;
    /* The attempt read past the `]` only if a `(` or a `[` stood there —
     * every other byte ends it after one look, and one look at a byte that is
     * present is a decision for good. With one of those two behind it, the
     * destination, title or label scan may have run to the buffer's end, so
     * the bracket and everything after it is a later scan's again. */
    if (initial_pos >= scan->input.len || scan->input.data[initial_pos] == '(' ||
        scan->input.data[initial_pos] == '[') {
        inline_parser_note_open(scan, opener_position);
    }
    return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));

match:
    if (is_reference) {
        inl = make_simple(scan->mem, is_image ? MARKDOWN_CORE_NODE_IMAGE_REFERENCE : MARKDOWN_CORE_NODE_LINK_REFERENCE);
    } else {
        inl = make_simple(scan->mem, is_image ? MARKDOWN_CORE_NODE_IMAGE : MARKDOWN_CORE_NODE_LINK);
    }
    if (!inl) {
        scan->oom = 1;
        markdown_core_chunk_free(scan->mem, &url);
        markdown_core_chunk_free(scan->mem, &title);
        markdown_core_chunk_free(scan->mem, &label);
        pop_bracket(scan);
        scan->pos = initial_pos;
        return make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("]"));
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
    inl->start_line = inl->end_line = scan->line;
    inl->start_column = opener->inl_text->start_column;
    inl->end_column = scan->pos + scan->column_offset + scan->block_offset;
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
    markdown_core_concrete_capture_consume_all(&scan->capture, opener->concrete_floor);
    capture_token(scan, MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, initial_pos - 1, 1, 1);
    if (scan->pos > initial_pos) {
        capture_token(
            scan,
            MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL,
            initial_pos,
            scan->pos - initial_pos,
            scan->pos - initial_pos
        );
    }

    process_delimiters(parser, scan, opener->delimiter_mark);
    pop_bracket(scan);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        scan->no_link_openers = true;
    }

    return NULL;
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the markdown_core_inline_parser has a cr or newline at the current position.
static markdown_core_node *handle_newline(markdown_core_inline_parser *scan) {
    markdown_core_bufsize nlpos = scan->pos;
    // skip over cr, crlf, or lf:
    if (peek_at(scan, scan->pos) == '\r') {
        advance(scan);
    }
    if (peek_at(scan, scan->pos) == '\n') {
        advance(scan);
    }
    ++scan->line;
    scan->column_offset = -scan->pos;
    // skip spaces at beginning of line
    skip_spaces(scan);
    if (nlpos > 1 && peek_at(scan, nlpos - 1) == ' ' && peek_at(scan, nlpos - 2) == ' ') {
        return make_simple_subj(scan, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        return make_simple_subj(scan, MARKDOWN_CORE_NODE_SOFT_BREAK);
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

static markdown_core_bufsize inline_parser_find_special_char(markdown_core_inline_parser *scan, int options) {
    markdown_core_bufsize n = scan->pos + 1;

    while (n < scan->input.len) {
        if (scan->special_chars[scan->input.data[n]]) {
            return n;
        }
        if (options & MARKDOWN_CORE_OPT_SMART && SMART_PUNCT_CHARS[scan->input.data[n]]) {
            return n;
        }
        n++;
    }

    return scan->input.len;
}

static markdown_core_node *call_inline_attachment(
    markdown_core_inline_attachment *attachment,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char c,
    markdown_core_inline_parser *scan
) {
    markdown_core_node *result;
    markdown_core_bufsize start = scan->pos;

    if (scan->phase != INLINE_PHASE_SCAN || scan->active_attachment) {
        scan->internal_error = 1;
        return NULL;
    }
    scan->active_attachment = attachment;
    result = attachment->extension->match_inline(attachment->extension, parser, parent, c, scan);
    scan->active_attachment = NULL;

    if (parser->oom) {
        scan->oom = 1;
    }
    if (parser->internal_error) {
        scan->internal_error = 1;
    }
    if ((result && (scan->pos <= start || scan->pos > scan->input.len)) || (!result && scan->pos != start)) {
        scan->internal_error = 1;
    }
    if ((scan->oom || scan->internal_error) && result) {
        markdown_core_node_free(result);
        result = NULL;
    }
    return result;
}

static markdown_core_node *try_extensions(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char c,
    markdown_core_inline_parser *scan
) {
    const markdown_core_inline_dispatch *bucket = &parser->inline_config->dispatch[c];
    markdown_core_bufsize start = scan->pos;
    size_t i;
    /* WHAT AN EXTENSION LOOKED AT DECIDES THE FRONTIER HERE. A handler scans
     * forward the way `$`, `[[` and `:name{}` do — one that DECLINED may
     * have declined only because its closer had not arrived, and one that
     * SUCCEEDED may have succeeded on a truncated construct that more bytes
     * complete differently (`:dir{a=b` on its own line, completed by a `}`
     * later in the unit, is the witness) — so what matters is not the
     * answer but how far the scan READ.
     *
     * The engine cannot see an extension's own scanners, so each handler
     * says: markdown_core_inline_parser_note_read, the byte after the last
     * one it examined. A scan that reached the buffer's end decides
     * differently once the buffer grows, so everything from this byte on is
     * a later scan's to derive again; a scan that stopped short decided on
     * bytes it could all see, and decided for good.
     *
     * SILENCE IS "TO THE END", which is exactly what every handler was
     * assumed to do before any of them could speak: an extension that says
     * nothing costs its unit's frontier what it always did, and one that
     * speaks buys the settled prefix back. That is why this contract needs
     * no attach-time refusal the way `opaque_size` does — its default is
     * the sound one. */
    for (i = 0; i < bucket->count; i++) {
        markdown_core_node *result;
        scan->read_end = 0;
        result = call_inline_attachment(bucket->items[i], parser, parent, c, scan);
        if (scan->read_end == 0 || scan->read_end >= scan->input.len) {
            inline_parser_note_open(scan, start);
        }
        scan->read_end = 0;
        if (result || scan->oom || scan->internal_error) {
            return result;
        }
    }
    return NULL;
}

static int inline_parser_has_failure(const markdown_core_parser *parser, const markdown_core_inline_parser *scan) {
    return scan->oom || scan->internal_error || parser->oom || parser->internal_error;
}

typedef struct {
    markdown_core_node *node;
    int handled;
} shared_close_result;

static markdown_core_node *consume_rule_close(
    markdown_core_inline_parser *scan,
    const markdown_core_delimiter_binding *binding,
    markdown_core_bufsize length
) {
    markdown_core_bufsize start = scan->pos;
    markdown_core_bufsize end;
    markdown_core_inline_source_span span;
    markdown_core_node *node;
    markdown_core_delimiter_result result;

    if (!binding || !binding->rule || !binding->rule->close_probe || length <= 0 || length > scan->input.len - start ||
        scan->input.data[start] != binding->rule->close_trigger) {
        scan->internal_error = 1;
        return NULL;
    }
    end = start + length;
    if (!source_span_through(scan, end, &span)) {
        return NULL;
    }
    node = stage_source_text(scan, start, end, &span);
    if (!node) {
        return NULL;
    }
    result = markdown_core_delimiter_engine_push(scan->delimiters, binding, 0, 1, node, start, end, 0);
    if (result != MARKDOWN_CORE_DELIMITER_OK) {
        markdown_core_node_free(node);
        inline_parser_set_delimiter_failure(scan, result);
        return NULL;
    }
    commit_source_span(scan, end, &span);
    return node;
}

/*
 * A shared close belongs to the newest semantic opener whose pure lexical
 * probe accepts the current bytes. Every rule is probed at most once; only
 * the winning binding can consume, allocate, or mutate the delimiter chain.
 */
static shared_close_result handle_shared_close(
    markdown_core_parser *parser,
    markdown_core_inline_parser *scan,
    unsigned char trigger
) {
    shared_close_result result = {NULL, 0};
    const markdown_core_inline_close_dispatch *bucket = &parser->inline_config->close_dispatch[trigger];
    const markdown_core_delimiter_binding *winner = NULL;
    markdown_core_bufsize winner_length = 0;
    uint64_t winner_order = 0;
    int bracket_wins = 0;
    size_t i;

    for (i = 0; i < bucket->count; i++) {
        const markdown_core_delimiter_binding *binding = bucket->items[i];
        markdown_core_delimiter_id opener = markdown_core_delimiter_engine_last_open(scan->delimiters, binding);
        markdown_core_bufsize length;
        uint64_t order;
        if (!opener) {
            continue;
        }
        order = markdown_core_delimiter_engine_claim_order(scan->delimiters, opener);
        if (!order) {
            scan->internal_error = 1;
            result.handled = 1;
            return result;
        }
        length = binding->rule->close_probe(binding->local_kind, scan->input.data, scan->input.len, scan->pos);
        if (length < 0 || length > scan->input.len - scan->pos) {
            scan->internal_error = 1;
            result.handled = 1;
            return result;
        }
        if (length > 0) {
            if (order == winner_order) {
                scan->internal_error = 1;
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

    if (trigger == ']' && scan->last_bracket) {
        if (scan->last_bracket->claim_order == winner_order) {
            scan->internal_error = 1;
            result.handled = 1;
            return result;
        }
        if (scan->last_bracket->claim_order > winner_order) {
            winner = NULL;
            winner_order = scan->last_bracket->claim_order;
            bracket_wins = 1;
        }
    }
    if (!winner_order) {
        return result;
    }

    result.handled = 1;
    if (bracket_wins) {
        markdown_core_bufsize before = scan->pos;
        result.node = handle_close_bracket(parser, scan);
        if (!scan->oom && !scan->internal_error && (scan->pos <= before || scan->pos > scan->input.len)) {
            scan->internal_error = 1;
        }
        return result;
    }

    result.node = consume_rule_close(scan, winner, winner_length);
    return result;
}

// Parse an inline, advancing markdown_core_inline_parser, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
static int parse_inline(
    markdown_core_parser *parser,
    markdown_core_inline_parser *scan,
    markdown_core_node *parent,
    int options
) {
    markdown_core_node *new_inl = NULL;
    markdown_core_chunk contents;
    shared_close_result shared_close;
    unsigned char c;
    markdown_core_bufsize startpos, endpos;
    c = peek_char(scan);
    if (c == 0) {
        return 0;
    }
    if (parser->inline_config->close_dispatch[c].count || (c == ']' && scan->last_bracket)) {
        shared_close = handle_shared_close(parser, scan, c);
        if (shared_close.handled) {
            new_inl = shared_close.node;
            goto parsed;
        }
    }
    switch (c) {
    case '\r':
    case '\n':
        new_inl = handle_newline(scan);
        break;
    case '`':
        new_inl = handle_backticks(scan, options);
        break;
    case '\\':
        new_inl = try_extensions(parser, parent, c, scan);
        if (!new_inl && inline_parser_has_failure(parser, scan)) {
            goto parsed;
        }
        if (new_inl == NULL) {
            new_inl = handle_backslash(parser, scan);
        }
        break;
    case '&':
        new_inl = handle_entity(scan);
        break;
    case '<':
        new_inl = handle_pointy_brace(scan, options);
        break;
    case '*':
    case '_':
    case '\'':
    case '"':
        new_inl = handle_delim(scan, c, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '-':
        new_inl = handle_hyphen(scan, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '.':
        new_inl = handle_period(scan, (options & MARKDOWN_CORE_OPT_SMART) != 0);
        break;
    case '[':
        new_inl = try_extensions(parser, parent, c, scan);
        if (!new_inl && inline_parser_has_failure(parser, scan)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }
        advance(scan);
        new_inl = make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("["));
        if (new_inl) {
            push_bracket(scan, false, new_inl);
        }
        break;
    case ']':
        new_inl = try_extensions(parser, parent, c, scan);
        if (!new_inl && !inline_parser_has_failure(parser, scan)) {
            new_inl = handle_close_bracket(parser, scan);
        }
        break;
    case '!':
        new_inl = try_extensions(parser, parent, c, scan);
        if (!new_inl && inline_parser_has_failure(parser, scan)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }

        advance(scan);
        if (peek_char(scan) == '[' && peek_char_n(scan, 1) != '^') {
            advance(scan);
            new_inl = make_str(scan, scan->pos - 2, scan->pos - 1, markdown_core_chunk_literal("!["));
            if (new_inl) {
                push_bracket(scan, true, new_inl);
            }
        } else {
            new_inl = make_str(scan, scan->pos - 1, scan->pos - 1, markdown_core_chunk_literal("!"));
        }
        break;
    default:
        new_inl = try_extensions(parser, parent, c, scan);
        if (!new_inl && inline_parser_has_failure(parser, scan)) {
            goto parsed;
        }
        if (new_inl != NULL) {
            break;
        }

        endpos = inline_parser_find_special_char(scan, options);
        contents = markdown_core_chunk_borrow(&scan->input, scan->pos, endpos - scan->pos);
        startpos = scan->pos;
        scan->pos = endpos;

        // if we're at a newline, strip trailing spaces.
        if (S_is_line_end_char(peek_char(scan))) {
            markdown_core_chunk_rtrim(&contents);
        }

        new_inl = make_str(scan, startpos, endpos - 1, contents);
    }
parsed:
    if (inline_parser_has_failure(parser, scan)) {
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
/* A CHILD THAT SETTLES TAKES ITS BYTES WITH IT. Its literals borrow the
 * unit's content buffer, and the next feed appends to that buffer and may
 * move it — where a child derived again every tick never outlives the
 * buffer it read. The copy is one per child for the life of the stream,
 * paid where the child settles; deriving it again is one per child per
 * tick. Answers false when a copy could not be allocated. */
static bool inline_own_subtree(markdown_core_node *root) {
    markdown_core_node *node = root;
    for (;;) {
        if (!markdown_core_node_own_chunks(node)) {
            return false;
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        if (node == root) {
            return true;
        }
        node = node->next;
    }
}

/* The children a settle point just proved: everything after the prefix the
 * last settle left, through the youngest child. */
static bool inline_own_settled(markdown_core_node *parent, markdown_core_node *after) {
    markdown_core_node *child = after ? after->next : parent->first_child;
    for (; child; child = child->next) {
        if (!inline_own_subtree(child)) {
            return false;
        }
    }
    return true;
}

void markdown_core_parse_inlines(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_map *refmap,
    int options
) {
    markdown_core_inline_parser scan;
    markdown_core_chunk content = {parent->content.ptr, parent->content.size, 0};
    const markdown_core_node *refining = parser->refining;
    /* THE PREFIX THIS REFINE KEEPS. The frontier the parser carries is this
     * unit's only if it names it; anything else begins at zero, and what
     * this refine proves takes its place when it ends. */
    markdown_core_inline_frontier settled = parser->inline_frontier;
    markdown_core_node *began_after;
    bool resume;
    /* The input the scan will read, trailing whitespace and all its line
     * endings cut: what the prefix must lie inside. */
    markdown_core_chunk_rtrim(&content);
    /* THE REFINE IS THE ONLY AUTHORITY ON THE PREFIX. The retract keeps it
     * optimistically — it is right nearly always and costs nothing to keep
     * — but only here is the input the scan will actually read known: the
     * held line the retract took off has come back or has not, and the
     * trailing whitespace the trim cuts may now stand where the prefix
     * ended. A prefix the scan cannot continue is DERIVED data, so it goes
     * and the unit is built from zero; keeping it while parsing from zero
     * would publish it twice. */
    resume = settled.unit == parent && settled.content > 0 && settled.content <= content.len;
    began_after = resume ? settled.last_child : NULL;
    if (!resume) {
        memset(&settled, 0, sizeof(settled));
        if (parent->first_child) {
            markdown_core_parser_drop_inline_prefix(parser, parent);
        }
    }
    inline_parser_from_buf(
        parser,
        parser->mem,
        resume ? settled.line : parent->start_line,
        parent->start_column - 1 + parent->internal_offset,
        &scan,
        &content,
        refmap
    );
    if (resume) {
        /* The scan stands where it stood when the settle point was taken —
         * a line start with nothing open behind it — so it reads on from
         * there and appends to the children already under the unit. The
         * records it captures continue the vector the prefix filled, and
         * the labels the prefix asked are asked again in its name, so what
         * this refine hands over covers the whole unit. */
        size_t i;
        scan.pos = settled.content;
        scan.column_offset = settled.column;
        scan.settle_at = settled.content;
        markdown_core_concrete_capture_adopt(&scan.capture, parent->inline_concrete);
        parent->inline_concrete = NULL;
        for (i = 0; parent->probes && i < settled.probes && i < parent->probes->count; i++) {
            S_record_probe(parser, parent->probes->links[i].hash);
        }
    }

    /* Diagnostics raised in here are this unit's, so a refine that is
     * undone takes them with it. */
    parser->refining = parent;
    while (!is_eof(&scan) && parse_inline(parser, &scan, parent, options)) {
        inline_parser_settle_delimiters(parser, &scan);
        if (scan.settle_at > settled.content) {
            if (!inline_own_settled(parent, settled.last_child)) {
                scan.oom = 1;
                break;
            }
            settled.unit = parent;
            settled.last_child = parent->last_child;
            settled.content = scan.settle_at;
            settled.line = scan.line;
            settled.column = scan.column_offset;
            settled.concrete = markdown_core_concrete_capture_count(&scan.capture);
            settled.probes = parser->probe_count;
            settled.diagnostics = parser->diagnostic_count;
        }
    }
    parser->refining = refining;
    /* Where this refine began, for the walks that follow it (the
     * postprocess, the consolidation, an extension's own); where the NEXT
     * one may begin is `last_child`. */
    settled.begin_child = began_after;
    /* What this refine proved and where it began. The caller that knows
     * whether this unit is still growing promotes it (parser.h). */
    parser->inline_refine = settled;
    parser->inline_refine.unit = parent;
    parser->inline_refine.begin_child = began_after;

    process_delimiters(parser, &scan, (markdown_core_delimiter_mark){0, 0, 0});
    // free bracket stack
    while (scan.last_bracket) {
        pop_bracket(&scan);
    }

    if (scan.oom) {
        parser->oom = true;
    }
    if (scan.internal_error) {
        parser->internal_error = true;
    }
    /* Handoff: the parsed node owns its inline records from here — through
     * adoption, the dependent-domain swap, and detach. A failed parse is
     * discarded whole, records included, so a transient loss can never
     * publish a quietly thinner tree. */
    if (inline_parser_has_failure(parser, &scan)) {
        markdown_core_concrete_capture_abandon(&scan.capture);
    } else {
        /* Each inline-owning node is parsed exactly once per refine — a
         * flip or a retract that refines it again frees its records first —
         * so nothing is ever overwritten here. */
        parent->inline_concrete = markdown_core_concrete_capture_take(&scan.capture);
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
static void spnl(markdown_core_inline_parser *scan) {
    skip_spaces(scan);
    if (skip_line_end(scan)) {
        skip_spaces(scan);
    }
}

// Parse reference.  Assumes string begins with '[' character.
// Modify refmap if a reference is encountered.
// Return 0 if no reference found, otherwise position of markdown_core_inline_parser
// after reference is parsed.
void markdown_core_inline_parser_note_read(markdown_core_inline_parser *parser, int end) {
    markdown_core_bufsize at = (markdown_core_bufsize)end;
    if (!parser) {
        return;
    }
    if (at < 0) {
        at = 0;
    }
    if (at > parser->input.len) {
        at = parser->input.len;
    }
    /* The furthest of everything this handler looked at. */
    if (at > parser->read_end) {
        parser->read_end = at;
    }
}

markdown_core_bufsize markdown_core_parse_reference_inline(
    markdown_core_mem *mem,
    markdown_core_chunk *input,
    markdown_core_map *refmap,
    markdown_core_reference_spans *spans
) {
    markdown_core_inline_parser scan;

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

    inline_parser_from_buf(NULL, mem, -1, 0, &scan, input, NULL);

    // parse label:
    if (!link_label(&scan, &lab) || lab.len == 0) {
        return 0;
    }

    // colon:
    if (peek_char(&scan) == ':') {
        advance(&scan);
        spans->label_end = scan.pos;
    } else {
        return 0;
    }

    // parse link url:
    spnl(&scan);
    if ((matchlen = manual_scan_link_url(&scan.input, scan.pos, &url)) > -1) {
        spans->url_start = scan.pos;
        spans->url_end = scan.pos + matchlen;
        scan.pos += matchlen;
    } else {
        return 0;
    }

    // parse optional link_title
    beforetitle = scan.pos;
    spnl(&scan);
    matchlen = scan.pos == beforetitle ? 0 : scan_link_title(&scan.input, scan.pos);
    if (matchlen) {
        title = markdown_core_chunk_borrow(&scan.input, scan.pos, matchlen);
        spans->title_start = scan.pos;
        spans->title_end = scan.pos + matchlen;
        scan.pos += matchlen;
    } else {
        scan.pos = beforetitle;
        title = markdown_core_chunk_literal("");
    }

    // parse final spaces and newline:
    skip_spaces(&scan);
    if (!skip_line_end(&scan)) {
        if (matchlen) { // try rewinding before title
            /* The rewound bytes return to the paragraph, so the definition
             * has no title at all — scalar, map entry, and spans agree
             * (CommonMark: "This is a link reference definition, but it
             * has no title"). Upstream cmark-gfm keeps the scanned title
             * in its map here, a registered deliberate difference. */
            scan.pos = beforetitle;
            title = markdown_core_chunk_literal("");
            spans->title_start = 0;
            spans->title_end = 0;
            skip_spaces(&scan);
            if (!skip_line_end(&scan)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    // insert reference into refmap
    markdown_core_reference_create(refmap, &lab, &url, &title);
    if (scan.oom && refmap) {
        refmap->oom = 1;
    }
    return scan.pos;
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
        claim_order = inline_parser_next_claim_order(parser);
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
        inline_parser_set_delimiter_failure(parser, result);
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
    /* Reducers only run inside a real parse, whose scan always engages
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
