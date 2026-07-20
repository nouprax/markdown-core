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
    bufsize_t position;
    bool image;
    bool active;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
} bracket;

#define FLAG_SKIP_HTML_CDATA (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION (1u << 1)
#define FLAG_SKIP_HTML_PI (1u << 2)
#define FLAG_SKIP_HTML_COMMENT (1u << 3)

typedef struct subject {
    markdown_core_mem *mem;
    markdown_core_chunk input;
    unsigned flags;
    int line;
    bufsize_t pos;
    int block_offset;
    int column_offset;
    markdown_core_map *refmap;
    delimiter *last_delim;
    bracket *last_bracket;
    bufsize_t backticks[MAXBACKTICKS + 1];
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

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static delimiter *S_insert_emph(subject *subj, delimiter *opener, delimiter *closer);

static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent, int options);

static void subject_from_buf(
    markdown_core_parser *parser,
    markdown_core_mem *mem,
    int line_number,
    int block_offset,
    subject *e,
    markdown_core_chunk *buffer,
    markdown_core_map *refmap
);
static bufsize_t subject_find_special_char(subject *subj, int options);

// Create an inline with a literal string value.
static MARKDOWN_CORE_INLINE markdown_core_node *
make_literal(subject *subj, markdown_core_node_type t, int start_column, int end_column, markdown_core_chunk s) {
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
static markdown_core_node *
make_str_with_entities(subject *subj, int start_column, int end_column, markdown_core_chunk *content) {
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

// Duplicate a chunk by creating a copy of the buffer not by reusing the
// buffer like markdown_core_chunk_dup does.
static markdown_core_chunk chunk_clone(subject *subj, markdown_core_chunk *src) {
    markdown_core_chunk c;
    bufsize_t len = src->len;

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

static MARKDOWN_CORE_INLINE markdown_core_node *
make_autolink(subject *subj, int start_column, int end_column, markdown_core_chunk url, int is_email) {
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
        append_child(link, text);
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
    e->special_chars = parser ? parser->special_chars : BASE_SPECIAL_CHARS;
    e->skip_chars = parser ? parser->skip_chars : BASE_SKIP_CHARS;
    e->mem = mem;
    e->input = *chunk;
    e->flags = 0;
    e->line = line_number;
    e->pos = 0;
    e->block_offset = block_offset;
    e->column_offset = 0;
    e->refmap = refmap;
    e->last_delim = NULL;
    e->last_bracket = NULL;
    for (i = 0; i <= MAXBACKTICKS; i++) {
        e->backticks[i] = 0;
    }
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

// Return the number of newlines in a given span of text in a subject.  If
// the number is greater than zero, also return the number of characters
// between the last newline and the end of the span in `since_newline`.
static int count_newlines(subject *subj, bufsize_t from, bufsize_t len, int *since_newline) {
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

// Adjust `node`'s `end_line`, `end_column`, and `subj`'s `line` and
// `column_offset` according to the number of newlines in a just-matched span
// of text in `subj`.
static void adjust_subj_node_newlines(subject *subj, markdown_core_node *node, int matchlen, int extra, int options) {
    if (!(options & MARKDOWN_CORE_OPT_SOURCEPOS)) {
        return;
    }

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
static bufsize_t scan_to_closing_backticks(subject *subj, bufsize_t openticklength) {

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
        bufsize_t numticks = 0;
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
static markdown_core_node *handle_backticks(subject *subj, int options) {
    markdown_core_chunk openticks = take_while(subj, isbacktick);
    bufsize_t startpos = subj->pos;
    bufsize_t endpos = scan_to_closing_backticks(subj, openticks.len);

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
        adjust_subj_node_newlines(subj, node, endpos - startpos, openticks.len, options);
        return node;
    }
}

// Scan ***, **, or * and return number scanned, or 0.
// Advances position.
static int scan_delims(subject *subj, unsigned char c, bool *can_open, bool *can_close) {
    int numdelims = 0;
    bufsize_t before_char_pos, after_char_pos;
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
        while (subj->skip_chars[peek_at(subj, after_char_pos)] && after_char_pos < subj->input.len) {
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

/*
static void print_delimiters(subject *subj)
{
        delimiter *delim;
        delim = subj->last_delim;
        while (delim != NULL) {
                printf("Item at stack pos %p: %d %d %d next(%p) prev(%p)\n",
                       (void*)delim, delim->delim_char,
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
    subj->mem->free(subj->mem, delim);
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

static void
push_delimiter(subject *subj, unsigned char c, bool can_open, bool can_close, markdown_core_node *inl_text) {
    delimiter *delim;
    /* Extensions may pass NULL after their own allocation failures. */
    if (!inl_text) {
        subj->oom = 1;
        return;
    }
    delim = (delimiter *)subj->mem->calloc(subj->mem, 1, sizeof(delimiter));
    if (!delim) {
        /* The literal text node stays in the tree; only its emphasis
         * potential is lost, which the sticky flag reports. */
        subj->oom = 1;
        return;
    }
    delim->delim_char = c;
    delim->can_open = can_open;
    delim->can_close = can_close;
    delim->inl_text = inl_text;
    delim->position = subj->pos;
    delim->length = inl_text->as.literal.len;
    delim->previous = subj->last_delim;
    delim->next = NULL;
    if (delim->previous != NULL) {
        delim->previous->next = delim;
    }
    subj->last_delim = delim;
}

static void push_bracket(subject *subj, bool image, markdown_core_node *inl_text) {
    bracket *b = (bracket *)subj->mem->calloc(subj->mem, 1, sizeof(bracket));
    if (!b) {
        subj->oom = 1;
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
    b->bracket_after = false;
    if (image) {
        b->in_bracket_image1 = true;
    } else {
        b->in_bracket_image0 = true;
    }
    subj->last_bracket = b;
    if (!image) {
        subj->no_link_openers = false;
    }
}

// Assumes the subject has a c at the current position.
static markdown_core_node *handle_delim(subject *subj, unsigned char c, bool smart) {
    bufsize_t numdelims;
    markdown_core_node *inl_text;
    bool can_open, can_close;
    markdown_core_chunk contents;

    numdelims = scan_delims(subj, c, &can_open, &can_close);

    if (c == '\'' && smart) {
        contents = markdown_core_chunk_literal(RIGHTSINGLEQUOTE);
    } else if (c == '"' && smart) {
        contents = markdown_core_chunk_literal(can_close ? RIGHTDOUBLEQUOTE : LEFTDOUBLEQUOTE);
    } else {
        contents = markdown_core_chunk_dup(&subj->input, subj->pos - numdelims, numdelims);
    }

    inl_text = make_str(subj, subj->pos - numdelims, subj->pos - 1, contents);

    if (inl_text && (can_open || can_close) && (!(c == '\'' || c == '"') || smart)) {
        push_delimiter(subj, c, can_open, can_close, inl_text);
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

static int extension_has_special_char(markdown_core_extension *ext, unsigned char c) {
    for (size_t i = 0; i < ext->special_inline_char_count; i++) {
        if (ext->special_inline_chars[i] == c) {
            return 1;
        }
    }

    return 0;
}

static markdown_core_extension *get_extension_for_special_char(markdown_core_parser *parser, unsigned char c) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        markdown_core_extension *ext = (markdown_core_extension *)tmp_ext->data;
        if (extension_has_special_char(ext, c)) {
            return ext;
        }
    }

    return NULL;
}

static void process_emphasis(markdown_core_parser *parser, subject *subj, bufsize_t stack_bottom) {
    delimiter *candidate;
    delimiter *closer = NULL;
    delimiter *opener;
    delimiter *old_closer;
    bool opener_found;
    bufsize_t openers_bottom[3][128];
    int i;

    // initialize openers_bottom:
    memset(&openers_bottom, 0, sizeof(openers_bottom));
    for (i = 0; i < 3; i++) {
        openers_bottom[i]['*'] = stack_bottom;
        openers_bottom[i]['_'] = stack_bottom;
        openers_bottom[i]['\''] = stack_bottom;
        openers_bottom[i]['"'] = stack_bottom;
    }

    // move back to first relevant delim.
    candidate = subj->last_delim;
    while (candidate != NULL && candidate->position >= stack_bottom) {
        closer = candidate;
        candidate = candidate->previous;
    }

    // now move forward, looking for closers, and handling each
    while (closer != NULL) {
        markdown_core_extension *extension = get_extension_for_special_char(parser, closer->delim_char);
        if (closer->can_close) {
            // Now look backwards for first matching opener:
            opener = closer->previous;
            opener_found = false;
            while (opener != NULL && opener->position >= stack_bottom &&
                   opener->position >= openers_bottom[closer->length % 3][closer->delim_char]) {
                if (opener->can_open && opener->delim_char == closer->delim_char) {
                    // interior closer of size 2 can't match opener of size 1
                    // or of size 1 can't match 2
                    if (!(closer->can_open || opener->can_close) || closer->length % 3 == 0 ||
                        (opener->length + closer->length) % 3 != 0) {
                        opener_found = true;
                        break;
                    }
                }
                opener = opener->previous;
            }
            old_closer = closer;

            if (extension) {
                if (opener_found) {
                    closer = extension->insert_inline_from_delim(extension, parser, subj, opener, closer);
                } else {
                    closer = closer->next;
                }
            } else if (closer->delim_char == '*' || closer->delim_char == '_') {
                if (opener_found) {
                    closer = S_insert_emph(subj, opener, closer);
                } else {
                    closer = closer->next;
                }
            } else if (closer->delim_char == '\'' || closer->delim_char == '"') {
                markdown_core_chunk_free(subj->mem, &closer->inl_text->as.literal);
                if (closer->delim_char == '\'') {
                    closer->inl_text->as.literal = markdown_core_chunk_literal(RIGHTSINGLEQUOTE);
                } else {
                    closer->inl_text->as.literal = markdown_core_chunk_literal(RIGHTDOUBLEQUOTE);
                }
                closer = closer->next;
                if (opener_found) {
                    markdown_core_chunk_free(subj->mem, &opener->inl_text->as.literal);
                    if (old_closer->delim_char == '\'') {
                        opener->inl_text->as.literal = markdown_core_chunk_literal(LEFTSINGLEQUOTE);
                    } else {
                        opener->inl_text->as.literal = markdown_core_chunk_literal(LEFTDOUBLEQUOTE);
                    }
                    remove_delimiter(subj, opener);
                    remove_delimiter(subj, old_closer);
                }
            }
            if (!opener_found) {
                // set lower bound for future searches for openers
                openers_bottom[old_closer->length % 3][old_closer->delim_char] = old_closer->position;
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
    // free all delimiters in list until stack_bottom:
    while (subj->last_delim != NULL && subj->last_delim->position >= stack_bottom) {
        remove_delimiter(subj, subj->last_delim);
    }
}

static delimiter *S_insert_emph(subject *subj, delimiter *opener, delimiter *closer) {
    delimiter *delim, *tmp_delim;
    bufsize_t use_delims;
    markdown_core_node *opener_inl = opener->inl_text;
    markdown_core_node *closer_inl = closer->inl_text;
    bufsize_t opener_num_chars = opener_inl->as.literal.len;
    bufsize_t closer_num_chars = closer_inl->as.literal.len;
    markdown_core_node *tmp, *tmpnext, *emph;

    // calculate the actual number of characters used from this closer
    use_delims = (closer_num_chars >= 2 && opener_num_chars >= 2) ? 2 : 1;

    // remove used characters from associated inlines.
    opener_num_chars -= use_delims;
    closer_num_chars -= use_delims;
    opener_inl->as.literal.len = opener_num_chars;
    closer_inl->as.literal.len = closer_num_chars;

    // free delimiters between opener and closer
    delim = closer->previous;
    while (delim != NULL && delim != opener) {
        tmp_delim = delim->previous;
        remove_delimiter(subj, delim);
        delim = tmp_delim;
    }

    // create new emph or strong, and splice it in to our inlines
    // between the opener and closer
    emph = use_delims == 1 ? make_emphasis(subj->mem) : make_strong(subj->mem);
    if (!emph) {
        /* Leave the (already shortened) literals in place unstyled; the
         * sticky flag reports the loss. */
        subj->oom = 1;
        return closer->next;
    }

    tmp = opener_inl->next;
    while (tmp && tmp != closer_inl) {
        tmpnext = tmp->next;
        markdown_core_node_unlink(tmp);
        append_child(emph, tmp);
        tmp = tmpnext;
    }
    markdown_core_node_insert_after(opener_inl, emph);

    emph->start_line = opener_inl->start_line;
    emph->end_line = closer_inl->end_line;
    emph->start_column = opener_inl->start_column;
    emph->end_column = closer_inl->end_column;

    // if opener has 0 characters, remove it and its associated inline
    if (opener_num_chars == 0) {
        markdown_core_node_free(opener_inl);
        remove_delimiter(subj, opener);
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
        // remove empty closer inline
        markdown_core_node_free(closer_inl);
        // remove closer from list
        tmp_delim = closer->next;
        remove_delimiter(subj, closer);
        closer = tmp_delim;
    }

    return closer;
}

// Parse backslash-escape or just a backslash, returning an inline.
static markdown_core_node *handle_backslash(markdown_core_parser *parser, subject *subj) {
    bufsize_t start = subj->pos;
    advance(subj);
    unsigned char nextchar = peek_char(subj);
    if ((parser->backslash_ispunct ? parser->backslash_ispunct : markdown_core_ispunct)(nextchar)) {
        if (nextchar == '\\' && get_extension_for_special_char(parser, '\\') == NULL) {
            bufsize_t end = start;
            while (end + 1 < subj->input.len && subj->input.data[end] == '\\' && subj->input.data[end + 1] == '\\') {
                end += 2;
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)subj->mem->calloc(subj->mem, (size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    memset(output, '\\', (size_t)output_len);
                    subj->pos = end;
                    return make_str(subj, start, end - 1, contents);
                }
            }
        }
        // only ascii symbols and newline can be escaped
        advance(subj);
        return make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_dup(&subj->input, subj->pos - 1, 1));
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
    bufsize_t len;

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
    bufsize_t matchlen = 0;
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
        contents = markdown_core_chunk_dup(&subj->input, subj->pos - 1, matchlen + 1);
        subj->pos += matchlen;
        markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
        adjust_subj_node_newlines(subj, node, matchlen, 1, options);
        return node;
    }

    if (options & MARKDOWN_CORE_OPT_LIBERAL_HTML_TAG) {
        matchlen = scan_liberal_html_tag(&subj->input, subj->pos);
        if (matchlen > 0) {
            contents = markdown_core_chunk_dup(&subj->input, subj->pos - 1, matchlen + 1);
            subj->pos += matchlen;
            markdown_core_node *node = make_raw_html(subj, subj->pos - matchlen - 1, subj->pos - 1, contents);
            adjust_subj_node_newlines(subj, node, matchlen, 1, options);
            return node;
        }
    }

    // if nothing matches, just return the opening <:
    return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("<"));
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

// Return a link, an image, or a literal close bracket.
static markdown_core_node *handle_close_bracket(markdown_core_parser *parser, subject *subj) {
    bufsize_t initial_pos, after_link_text_pos;
    bufsize_t endurl, starttitle, endtitle, endall;
    bufsize_t sps, n;
    markdown_core_reference *ref = NULL;
    markdown_core_chunk url_chunk, title_chunk;
    markdown_core_chunk url, title;
    bracket *opener;
    markdown_core_node *inl;
    markdown_core_chunk raw_label;
    int found_label;
    markdown_core_node *tmp, *tmpnext;
    bool is_image;

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

    if (found_label) {
        ref = (markdown_core_reference *)markdown_core_map_lookup(subj->refmap, &raw_label);
        markdown_core_chunk_free(subj->mem, &raw_label);
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

            // Before we got this far, the `handle_close_bracket` function may have
            // advanced the current state beyond our footnote's actual closing
            // bracket, ie if it went looking for a `link_label`.
            // Let's just rewind the subject's position:
            subj->pos = initial_pos;

            markdown_core_node *fnref = make_simple(subj->mem, MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE);
            if (!fnref) {
                subj->oom = 1;
                pop_bracket(subj);
                return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
            }

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
            //
            // (first, check for underflows)
            if ((fnref_start_column + 2) <= fnref_end_column) {
                fnref->as.literal = markdown_core_chunk_dup(literal, 1, (fnref_end_column - fnref_start_column) - 2);
            } else {
                fnref->as.literal = markdown_core_chunk_dup(literal, 1, 0);
            }

            fnref->start_line = fnref->end_line = subj->line;
            fnref->start_column = fnref_start_column;
            fnref->end_column = fnref_end_column;

            // we then replace the opener with this new fnref node, the net effect
            // being replacing the opening '[' text node with a `^footnote-ref]` node.
            markdown_core_node_insert_before(opener->inl_text, fnref);

            process_emphasis(parser, subj, opener->position);
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
    inl = make_simple(subj->mem, is_image ? MARKDOWN_CORE_NODE_IMAGE : MARKDOWN_CORE_NODE_LINK);
    if (!inl) {
        subj->oom = 1;
        markdown_core_chunk_free(subj->mem, &url);
        markdown_core_chunk_free(subj->mem, &title);
        pop_bracket(subj);
        subj->pos = initial_pos;
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("]"));
    }
    inl->as.link.url = url;
    inl->as.link.title = title;
    inl->start_line = inl->end_line = subj->line;
    inl->start_column = opener->inl_text->start_column;
    inl->end_column = subj->pos + subj->column_offset + subj->block_offset;
    markdown_core_node_insert_before(opener->inl_text, inl);
    // Add link text:
    tmp = opener->inl_text->next;
    while (tmp) {
        tmpnext = tmp->next;
        markdown_core_node_unlink(tmp);
        append_child(inl, tmp);
        tmp = tmpnext;
    }

    // Free the bracket [:
    markdown_core_node_free(opener->inl_text);

    process_emphasis(parser, subj, opener->position);
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
    bufsize_t nlpos = subj->pos;
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

static bufsize_t subject_find_special_char(subject *subj, int options) {
    bufsize_t n = subj->pos + 1;

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

void markdown_core_inlines_add_special_character(markdown_core_parser *parser, unsigned char c, bool emphasis) {
    if (is_core_special_character(c)) {
        return;
    }

    parser->special_chars[c] = 1;
    if (emphasis) {
        parser->skip_chars[c] = 1;
    }
}

void markdown_core_inlines_remove_special_character(markdown_core_parser *parser, unsigned char c, bool emphasis) {
    if (is_core_special_character(c)) {
        return;
    }

    parser->special_chars[c] = 0;
    if (emphasis) {
        parser->skip_chars[c] = 0;
    }
}

static markdown_core_node *
try_extensions(markdown_core_parser *parser, markdown_core_node *parent, unsigned char c, subject *subj) {
    markdown_core_node *res = NULL;
    markdown_core_llist *tmp;

    for (tmp = parser->inline_extensions; tmp; tmp = tmp->next) {
        markdown_core_extension *ext = (markdown_core_extension *)tmp->data;

        if (!extension_has_special_char(ext, c)) {
            continue;
        }

        res = ext->match_inline(ext, parser, parent, c, subj);

        if (res) {
            break;
        }
    }

    return res;
}

static delimiter *find_extension_opener_for_special_char(markdown_core_parser *parser, subject *subj, unsigned char c) {
    delimiter *delim = subj->last_delim;
    int closer_count[256];

    memset(closer_count, 0, sizeof(closer_count));

    while (delim) {
        markdown_core_extension *extension = get_extension_for_special_char(parser, delim->delim_char);

        if (extension && extension_has_special_char(extension, c)) {
            if (delim->can_close) {
                closer_count[delim->delim_char]++;
            } else if (delim->can_open) {
                if (closer_count[delim->delim_char] > 0) {
                    closer_count[delim->delim_char]--;
                } else {
                    return delim;
                }
            }
        }

        delim = delim->previous;
    }

    return NULL;
}

static int bracket_takes_close_bracket(markdown_core_parser *parser, subject *subj) {
    delimiter *extension_opener = find_extension_opener_for_special_char(parser, subj, ']');

    return subj->last_bracket && (!extension_opener || subj->last_bracket->position > extension_opener->position);
}

// Parse an inline, advancing subject, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
static int parse_inline(markdown_core_parser *parser, subject *subj, markdown_core_node *parent, int options) {
    markdown_core_node *new_inl = NULL;
    markdown_core_chunk contents;
    unsigned char c;
    bufsize_t startpos, endpos;
    c = peek_char(subj);
    if (c == 0) {
        return 0;
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
        advance(subj);
        new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("["));
        if (new_inl) {
            push_bracket(subj, false, new_inl);
        }
        break;
    case ']':
        if (bracket_takes_close_bracket(parser, subj)) {
            new_inl = handle_close_bracket(parser, subj);
            break;
        }
        new_inl = try_extensions(parser, parent, c, subj);
        if (new_inl == NULL) {
            new_inl = handle_close_bracket(parser, subj);
        }
        break;
    case '!':
        new_inl = try_extensions(parser, parent, c, subj);
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
        if (new_inl != NULL) {
            break;
        }

        endpos = subject_find_special_char(subj, options);
        contents = markdown_core_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
        startpos = subj->pos;
        subj->pos = endpos;

        // if we're at a newline, strip trailing spaces.
        if (S_is_line_end_char(peek_char(subj))) {
            markdown_core_chunk_rtrim(&contents);
        }

        new_inl = make_str(subj, startpos, endpos - 1, contents);
    }
    if (new_inl != NULL) {
        append_child(parent, new_inl);
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
bufsize_t markdown_core_inline_seam_prefix(
    const markdown_core_parser *parser,
    const unsigned char *a,
    bufsize_t a_len,
    const unsigned char *b,
    bufsize_t b_len,
    int options
) {
    bufsize_t limit = a_len < b_len ? a_len : b_len;
    bufsize_t i = 0;
    bufsize_t seam = 0;
    while (i < limit) {
        unsigned char c = a[i];
        if (c != b[i]) {
            break;
        }
        if (c == '\n') {
            seam = i + 1;
        } else if (c != '\r') {
            if (parser->special_chars[c]) {
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
    bufsize_t start
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
        bufsize_t i;
        bufsize_t bound = start < subj.input.len ? start : subj.input.len;
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

    process_emphasis(parser, &subj, 0);
    // free bracket and delim stack
    while (subj.last_delim) {
        remove_delimiter(&subj, subj.last_delim);
    }
    while (subj.last_bracket) {
        pop_bracket(&subj);
    }

    if (subj.oom) {
        parser->oom = true;
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
bufsize_t
markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input, markdown_core_map *refmap) {
    subject subj;

    markdown_core_chunk lab;
    markdown_core_chunk url;
    markdown_core_chunk title;

    bufsize_t matchlen = 0;
    bufsize_t beforetitle;

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
        title = markdown_core_chunk_dup(&subj.input, subj.pos, matchlen);
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

char *
markdown_core_inline_parser_take_while(markdown_core_inline_parser *parser, markdown_core_inline_predicate_func pred) {
    unsigned char c;
    bufsize_t startpos = parser->pos;
    bufsize_t len = 0;

    while ((c = peek_char(parser)) && (*pred)(c)) {
        advance(parser);
        len++;
    }

    return my_strndup((const char *)parser->input.data + startpos, len);
}

void markdown_core_inline_parser_push_delimiter(
    markdown_core_inline_parser *parser,
    unsigned char c,
    int can_open,
    int can_close,
    markdown_core_node *inl_text
) {
    push_delimiter(parser, c, can_open != 0, can_close != 0, inl_text);
}

void markdown_core_inline_parser_remove_delimiter(markdown_core_inline_parser *parser, delimiter *delim) {
    remove_delimiter(parser, delim);
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
        len = markdown_core_utf8proc_iterate(
            parser->input.data + before_char_pos,
            parser->pos - before_char_pos,
            &before_char
        );
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

void markdown_core_inline_parser_advance_offset(markdown_core_inline_parser *parser) { advance(parser); }

int markdown_core_inline_parser_get_offset(markdown_core_inline_parser *parser) { return parser->pos; }

void markdown_core_inline_parser_set_offset(markdown_core_inline_parser *parser, int offset) { parser->pos = offset; }

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
    for (bufsize_t i = 0; i < node->as.literal.len; i++) {
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
        bufsize_t remove = node->as.literal.len < (bufsize_t)n ? node->as.literal.len : (bufsize_t)n;
        node->as.literal.len -= remove;
        n -= (int)remove;
        S_update_text_sourcepos(node);
        node = node->prev;
    }
}

delimiter *markdown_core_inline_parser_get_last_delimiter(markdown_core_inline_parser *parser) {
    return parser->last_delim;
}

int markdown_core_inline_parser_get_line(markdown_core_inline_parser *parser) { return parser->line; }
